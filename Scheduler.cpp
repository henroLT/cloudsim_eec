//
//  Scheduler.cpp — Advanced Snooze implementation
//

#include "Scheduler.hpp"
#include <algorithm>
#include <string>

// Cascading Sleep Timers
static constexpr Time_t SLEEP_S0i1_AFTER     =  15'000'000;   // 15 s
static constexpr Time_t SLEEP_S1_AFTER       =  60'000'000;   // 1 m
static constexpr Time_t SLEEP_S2_AFTER       = 120'000'000;   // 2 m
static constexpr Time_t SLEEP_S3_AFTER       = 300'000'000;   // 5 m
static constexpr Time_t SLEEP_S4_AFTER       = 600'000'000;   // 10 m
static constexpr Time_t SLEEP_S5_AFTER       = 900'000'000;   // 15 m

static constexpr double UNDERLOAD_THRESHOLD  = 0.20;  
static constexpr double OVERLOAD_THRESHOLD   = 0.80;  
static constexpr Time_t CONSOLIDATION_INTERVAL = 30'000'000; 


static double GetUtil(const MachineInfo_t& info) {
    if (info.num_cpus == 0) return 0.0;
    return (double)info.active_tasks / info.num_cpus;
}

static unsigned MemAvail(const MachineInfo_t& info) {
    return info.memory_size - info.memory_used;
}

unsigned Scheduler::VMMemFootprint(VMId_t vm) const {
    VMInfo_t vi = VM_GetInfo(vm);
    unsigned mem = VM_MEMORY_OVERHEAD;
    for (TaskId_t t : vi.active_tasks)
        mem += GetTaskInfo(t).required_memory;
    return mem;
}

bool Scheduler::HasInboundMigration(MachineId_t m) const {
    for (const auto& [vm, pair] : migrating_vms)
        if (pair.second == m) return true;
    return false;
}


void Scheduler::Init() {
    unsigned total = Machine_GetTotal();
    
    machines_by_cpu.reserve(4);
    vms_on_machine.reserve(total);
    pending_tasks.reserve(total);
    vm_types.reserve(total * 2);
    waking_machines.reserve(total / 2);
    powering_down.reserve(total / 2);

    for (unsigned i = 0; i < total; ++i) {
        MachineId_t cur = MachineId_t(i);
        machines_by_cpu[Machine_GetCPUType(cur)].push_back(cur);
        empty_since[cur] = 0;
        pending_memory[cur] = 0;
    }
}


void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    auto it = migrating_vms.find(vm_id);
    if (it == migrating_vms.end()) return;

    MachineId_t src = it->second.first;
    MachineId_t dst = it->second.second;
    migrating_vms.erase(it);

    auto& src_vms = vms_on_machine[src];
    src_vms.erase(std::remove(src_vms.begin(), src_vms.end(), vm_id), src_vms.end());
    vms_on_machine[dst].push_back(vm_id);

    if (src_vms.empty() && !empty_since.count(src)) {
        if (!HasInboundMigration(src)) {
            MachineInfo_t mi = Machine_GetInfo(src);
            if (mi.s_state == S0 && !waking_machines.count(src) && !powering_down.count(src))
                empty_since[src] = time;
        }
    }
}

// Snooze Algorithm 2 — Underload relocation

bool Scheduler::UnderloadRelocate(MachineId_t src_id, Time_t now) {
    auto& vms = vms_on_machine[src_id];
    if (vms.empty()) return false;

    MachineInfo_t src_info = Machine_GetInfo(src_id);

    std::vector<VMId_t> candidates;
    for (VMId_t vm : vms) {
        if (migrating_vms.count(vm)) return false; 
        
        VMInfo_t vi = VM_GetInfo(vm);
        bool sensitive = false;
        for (TaskId_t t : vi.active_tasks) {
            if (GetTaskInfo(t).required_sla == SLA0) { sensitive = true; break; }
        }
        if (!sensitive) candidates.push_back(vm);
    }
    
    if (candidates.empty()) return false;

    std::sort(candidates.begin(), candidates.end(), [this](VMId_t a, VMId_t b) {
        return VMMemFootprint(a) > VMMemFootprint(b);
    });

    std::vector<MachineId_t> dsts;
    for (MachineId_t m : machines_by_cpu[src_info.cpu]) {
        if (m == src_id) continue;
        MachineInfo_t mi = Machine_GetInfo(m);
        // Exclude transitional machines from targets
        if (mi.s_state != S0 || waking_machines.count(m) || powering_down.count(m)) continue;
        if (HasInboundMigration(m)) continue;
        dsts.push_back(m);
    }
    
    std::sort(dsts.begin(), dsts.end(), [](MachineId_t a, MachineId_t b) {
        return GetUtil(Machine_GetInfo(a)) > GetUtil(Machine_GetInfo(b));
    });

    std::vector<std::pair<VMId_t, MachineId_t>> plan;
    std::unordered_map<MachineId_t, unsigned> committed_mem; 
    
    for (VMId_t vm : candidates) {
        unsigned needed = VMMemFootprint(vm);
        bool placed = false;
        
        for (MachineId_t dst : dsts) {
            MachineInfo_t di = Machine_GetInfo(dst);
            unsigned avail = MemAvail(di) - committed_mem[dst];
            
            if (GetUtil(di) >= OVERLOAD_THRESHOLD) continue;
            if (avail < needed) continue;
            
            plan.push_back({vm, dst});
            committed_mem[dst] += needed;
            placed = true;
            break;
        }
        if (!placed) return false; 
    }

    for (auto& [vm, dst] : plan) {
        VM_Migrate(vm, dst);
        migrating_vms[vm] = {src_id, dst};
        empty_since.erase(dst); 
    }
    return true;
}

// Snooze Algorithm 3 — Sercon consolidation
void Scheduler::Consolidate(Time_t now) {
    std::vector<MachineId_t> active;
    for (auto& [cpu, machines] : machines_by_cpu) {
        for (MachineId_t m : machines) {
            MachineInfo_t mi = Machine_GetInfo(m);
            if (mi.s_state != S0 || waking_machines.count(m) || powering_down.count(m)) continue;
            if (vms_on_machine[m].empty()) continue;
            if (HasInboundMigration(m)) continue;
            active.push_back(m);
        }
    }

    std::sort(active.begin(), active.end(), [](MachineId_t a, MachineId_t b) {
        return GetUtil(Machine_GetInfo(a)) > GetUtil(Machine_GetInfo(b));
    });

    std::unordered_map<MachineId_t, unsigned> committed_mem;

    for (int i = (int)active.size() - 1; i >= 1; --i) {
        MachineId_t src_id = active[i];
        auto& src_vms = vms_on_machine[src_id];
        if (src_vms.empty()) continue;

        bool busy = false;
        for (VMId_t vm : src_vms)
            if (migrating_vms.count(vm)) { busy = true; break; }
        if (busy) continue;

        std::vector<VMId_t> candidates = src_vms;
        std::sort(candidates.begin(), candidates.end(), [this](VMId_t a, VMId_t b) {
            return VMMemFootprint(a) > VMMemFootprint(b);
        });

        std::vector<std::pair<VMId_t, MachineId_t>> plan;
        bool all_placed = true;

        for (VMId_t vm : candidates) {
            VMInfo_t vi = VM_GetInfo(vm);
            bool sensitive = false;
            for (TaskId_t t : vi.active_tasks) {
                SLAType_t sla = GetTaskInfo(t).required_sla;
                if (sla == SLA0 || sla == SLA1) { sensitive = true; break; }
            }
            if (sensitive) { all_placed = false; break; }

            unsigned needed = VMMemFootprint(vm);
            bool placed = false;

            for (int j = 0; j < i; ++j) {
                MachineId_t dst_id = active[j];
                MachineInfo_t di = Machine_GetInfo(dst_id);
                unsigned avail = MemAvail(di) - committed_mem[dst_id];

                if (di.s_state != S0 || waking_machines.count(dst_id) || powering_down.count(dst_id)) continue;
                if (HasInboundMigration(dst_id)) continue;
                if (GetUtil(di) >= OVERLOAD_THRESHOLD) continue;
                if (avail < needed) continue;
                
                plan.push_back({vm, dst_id});
                committed_mem[dst_id] += needed;
                placed = true;
                break;
            }
            if (!placed) { all_placed = false; break; }
        }

        if (all_placed && !plan.empty()) {
            for (auto& [vm, dst] : plan) {
                VM_Migrate(vm, dst);
                migrating_vms[vm] = {src_id, dst};
                empty_since.erase(dst);
            }
        }
    }
}


void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    const auto& candidates = machines_by_cpu[info.required_cpu];

    std::vector<MachineId_t> active_S0;
    std::vector<MachineId_t> sleeping;
    std::vector<MachineId_t> waking;

    unsigned mem_needed_base = info.required_memory; 

    // Sort into operational categories
    for (MachineId_t m : candidates) {
        if (powering_down.count(m)) continue; // Never touch machines going to sleep

        MachineInfo_t mi = Machine_GetInfo(m);
        unsigned mem_needed = mem_needed_base + VM_MEMORY_OVERHEAD;
        
        bool found = false;
        if (mi.s_state == S0 && !waking_machines.count(m)) {
            for (VMId_t vm : vms_on_machine[m]) {
                if (migrating_vms.count(vm)) continue;
                if (vm_types[vm] == info.required_vm) { found = true; break; }
            }
        }
        if (found) mem_needed = mem_needed_base;

        unsigned pending = pending_memory.count(m) ? pending_memory[m] : 0;
        if (mi.memory_size < mi.memory_used + pending + mem_needed) continue;

        if (waking_machines.count(m)) { waking.push_back(m); continue; }
        if (mi.s_state == S0) { active_S0.push_back(m); }
        else { sleeping.push_back(m); }
    }

    // Sort active machines
    auto sort_active = [&](MachineId_t a, MachineId_t b) {
        MachineInfo_t miA = Machine_GetInfo(a);
        MachineInfo_t miB = Machine_GetInfo(b);
        if (info.gpu_capable) {
            if (miA.gpus && !miB.gpus) return true;
            if (!miA.gpus && miB.gpus) return false;
        }
        return GetUtil(miA) > GetUtil(miB); 
    };
    std::sort(active_S0.begin(), active_S0.end(), sort_active);

    // Override Priorities
    Priority_t override_priority = info.priority;
    if (info.required_sla == SLA0) override_priority = HIGH_PRIORITY;
    else if (info.required_sla == SLA1) override_priority = MID_PRIORITY;
    else if (info.required_sla == SLA2) override_priority = LOW_PRIORITY;

    auto attach_to_active = [&](MachineId_t m) {
        empty_since.erase(m);
        VMId_t found_vm = 0;
        for (VMId_t vm : vms_on_machine[m]) {
            if (migrating_vms.count(vm)) continue;
            if (vm_types[vm] == info.required_vm) { found_vm = vm; break; }
        }
        if (!found_vm) {
            found_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(found_vm, m);
            vms_on_machine[m].push_back(found_vm);
            vm_types[found_vm] = info.required_vm;
        }
        VM_AddTask(found_vm, task_id, override_priority);
        if (info.required_sla == SLA0 || info.required_sla == SLA1) {
            unsigned nc = Machine_GetInfo(m).num_cpus;
            for (unsigned i = 0; i < nc; ++i) Machine_SetCorePerformance(m, i, P0);
        }
    };

    auto wake_machine = [&](MachineId_t m) {
        Machine_SetState(m, S0);
        waking_machines.insert(m);
        empty_since.erase(m);
        pending_tasks[m].push_back(task_id);
        pending_memory[m] += mem_needed_base + VM_MEMORY_OVERHEAD;
    };

    // Phase 1: Optimal S0 (Guaranteed Free Core)
    for (MachineId_t m : active_S0) {
        if (GetUtil(Machine_GetInfo(m)) < 1.0) {
            attach_to_active(m);
            return;
        }
    }

    // Phase 2: Shallow Sleepers (Instant/Fast Wake)
    MachineId_t best_shallow = MachineId_t(-1);
    MachineState_t best_shallow_state = S5;
    for (MachineId_t m : sleeping) {
        MachineInfo_t mi = Machine_GetInfo(m);
        if (mi.s_state <= S1) {
            if (mi.s_state <= best_shallow_state) { 
                best_shallow_state = mi.s_state;
                best_shallow = m;
            }
        }
    }
    if (best_shallow != MachineId_t(-1)) {
        wake_machine(best_shallow);
        return;
    }

    // Phase 3: Cram on S0 (Pick least utilized machine to minimize preemption overlap)
    if (!active_S0.empty()) {
        MachineId_t best_cram = active_S0.back(); 
        attach_to_active(best_cram);
        return;
    }

    // Phase 4: Piggyback on a waking machine
    if (!waking.empty()) {
        MachineId_t m = waking.front();
        pending_tasks[m].push_back(task_id);
        pending_memory[m] += mem_needed_base + VM_MEMORY_OVERHEAD;
        return;
    }

    // Phase 5: Deep Sleepers (Desperation Wake)
    MachineId_t best_deep = MachineId_t(-1);
    MachineState_t best_deep_state = S5;
    for (MachineId_t m : sleeping) {
        MachineInfo_t mi = Machine_GetInfo(m);
        if (mi.s_state <= best_deep_state) { 
            best_deep_state = mi.s_state;
            best_deep = m;
        }
    }
    if (best_deep != MachineId_t(-1)) {
        wake_machine(best_deep);
        return;
    }

    SimOutput("Snooze::NewTask(): CRITICAL - OUT OF MEMORY for task " + std::to_string(task_id), 0);
}


void Scheduler::PeriodicCheck(Time_t now) {
    auto it = empty_since.begin();
    while (it != empty_since.end()) {
        MachineId_t m_id  = it->first;
        Time_t      elapsed = now - it->second;
        MachineInfo_t mi  = Machine_GetInfo(m_id);

        if (HasInboundMigration(m_id)) { ++it; continue; }

        if (elapsed >= SLEEP_S5_AFTER && mi.s_state == S4) {
            Machine_SetState(m_id, S5);
            powering_down.insert(m_id);
            it = empty_since.erase(it);
        }
        else if (elapsed >= SLEEP_S4_AFTER && mi.s_state == S3) {
            Machine_SetState(m_id, S4);
            powering_down.insert(m_id);
            ++it;
        }
        else if (elapsed >= SLEEP_S3_AFTER && mi.s_state == S2) {
            Machine_SetState(m_id, S3);
            powering_down.insert(m_id);
            ++it;
        }
        else if (elapsed >= SLEEP_S2_AFTER && mi.s_state == S1) {
            Machine_SetState(m_id, S2);
            powering_down.insert(m_id);
            ++it;
        }
        else if (elapsed >= SLEEP_S1_AFTER && mi.s_state == S0i1) {
            Machine_SetState(m_id, S1);
            powering_down.insert(m_id);
            ++it;
        }
        else if (elapsed >= SLEEP_S0i1_AFTER && mi.s_state == S0) {
            Machine_SetState(m_id, S0i1);
            powering_down.insert(m_id);
            ++it;
        }
        else { ++it; }
    }

    for (auto& [cpu, machines] : machines_by_cpu) {
        for (MachineId_t m_id : machines) {
            MachineInfo_t mi = Machine_GetInfo(m_id);
            if (mi.s_state != S0 || waking_machines.count(m_id) || powering_down.count(m_id)) continue;
            if (vms_on_machine[m_id].empty()) continue;
            if (GetUtil(mi) >= UNDERLOAD_THRESHOLD) continue;
            if (HasInboundMigration(m_id)) continue;
            UnderloadRelocate(m_id, now);
        }
    }

    if (now - last_consolidation >= CONSOLIDATION_INTERVAL) {
        last_consolidation = now;
        Consolidate(now);
    }
}


void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    for (auto& [m_id, vms] : vms_on_machine) {
        auto it = vms.begin();
        while (it != vms.end()) {
            VMId_t vm = *it;
            if (migrating_vms.count(vm)) { ++it; continue; }
            if (VM_GetInfo(vm).active_tasks.empty()) {
                VM_Shutdown(vm);
                vm_types.erase(vm);
                it = vms.erase(it);
            } else { ++it; }
        }

        if (vms.empty() && !empty_since.count(m_id)) {
            if (HasInboundMigration(m_id)) continue;
            MachineInfo_t mi = Machine_GetInfo(m_id);
            if (mi.s_state == S0 && !waking_machines.count(m_id) && !powering_down.count(m_id))
                empty_since[m_id] = now;
        }
    }
}


void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id) {
    powering_down.erase(machine_id);

    if (!waking_machines.count(machine_id)) return;
    if (Machine_GetInfo(machine_id).s_state != S0) return;

    waking_machines.erase(machine_id);
    pending_memory[machine_id] = 0; 

    for (TaskId_t task_id : pending_tasks[machine_id]) {
        TaskInfo_t info = GetTaskInfo(task_id);
        
        Priority_t override_priority = info.priority;
        if (info.required_sla == SLA0) override_priority = HIGH_PRIORITY;
        else if (info.required_sla == SLA1) override_priority = MID_PRIORITY;
        else if (info.required_sla == SLA2) override_priority = LOW_PRIORITY;

        VMId_t target_vm = 0;
        bool found = false;

        for (VMId_t vm_id : vms_on_machine[machine_id]) {
            if (migrating_vms.count(vm_id)) continue;
            if (vm_types[vm_id] == info.required_vm) {
                target_vm = vm_id; found = true; break;
            }
        }
        if (!found) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, machine_id);
            vms_on_machine[machine_id].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }
        VM_AddTask(target_vm, task_id, override_priority);
        
        if (info.required_sla == SLA0 || info.required_sla == SLA1) {
            unsigned nc = Machine_GetInfo(machine_id).num_cpus;
            for (unsigned i = 0; i < nc; ++i) Machine_SetCorePerformance(machine_id, i, P0);
        }
    }
    pending_tasks[machine_id].clear();
}


void Scheduler::Shutdown(Time_t time) {
    std::unordered_set<VMId_t> shut;
    for (auto& [machine, vms] : vms_on_machine)
        for (VMId_t vm : vms)
            if (!shut.count(vm)) { VM_Shutdown(vm); shut.insert(vm); }
}


static Scheduler GlobalScheduler;

void InitScheduler()                                    { GlobalScheduler.Init(); }
void HandleNewTask(Time_t t, TaskId_t id)               { GlobalScheduler.NewTask(t, id); }
void HandleTaskCompletion(Time_t t, TaskId_t id)        { GlobalScheduler.TaskComplete(t, id); }
void MigrationDone(Time_t t, VMId_t id)                 { GlobalScheduler.MigrationComplete(t, id); }
void SchedulerCheck(Time_t t)                           { GlobalScheduler.PeriodicCheck(t); }
void StateChangeComplete(Time_t t, MachineId_t id)      { GlobalScheduler.StateChangeComplete(t, id); }
void SLAWarning(Time_t, TaskId_t) {}
void MemoryWarning(Time_t t, MachineId_t id) {
    SimOutput("MemoryWarning on machine " + std::to_string(id), 0);
}

void SimulationComplete(Time_t time) {
    printf("SLA violation report\n");
    printf("SLA0: %f%%\n", GetSLAReport(SLA0));
    printf("SLA1: %f%%\n", GetSLAReport(SLA1));
    printf("SLA2: %f%%\n", GetSLAReport(SLA2));
    printf("Total Energy %fKW-Hour\n", Machine_GetClusterEnergy());
    printf("Simulation run finished in %f seconds\n", double(time)/1000000);
    GlobalScheduler.Shutdown(time);
}