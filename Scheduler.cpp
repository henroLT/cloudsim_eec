//
//  Scheduler.cpp
//  CloudSim


#include "Scheduler.hpp"

// ═══════════════════════════════════════════════════════════════════════════════
// Private helpers
// ═══════════════════════════════════════════════════════════════════════════════

// ── Memory accounting ─────────────────────────────────────────────────────────

unsigned Scheduler::getFreeMemory(MachineId_t m) {
    MachineInfo_t info = Machine_GetInfo(m);
    return (info.memory_size > info.memory_used)
           ? info.memory_size - info.memory_used
           : 0u;
}

// Sum of VM_MEMORY_OVERHEAD plus every task currently inside this VM.
// Used to calculate the true footprint of a VM when planning migrations.
unsigned Scheduler::getVMMemory(VMId_t vm_id) {
    VMInfo_t info = VM_GetInfo(vm_id);
    unsigned mem  = VM_MEMORY_OVERHEAD;
    for (TaskId_t t : info.active_tasks)
        mem += GetTaskMemory(t);
    return mem;
}

// ── Machine counting / selection ─────────────────────────────────────────────

// Returns the number of machines that are currently usable (S0 and not in the
// process of shutting down) OR that are on their way to S0 (waking_machines).
// We count waking machines so that "keep at least 1 hot" cannot accidentally
// keep a machine alive that is about to be replaced by a waking one.
unsigned Scheduler::countActiveMachines(CPUType_t cpu) {
    auto it = machines_by_cpu.find(cpu);
    if (it == machines_by_cpu.end()) return 0;

    unsigned count = 0;
    for (MachineId_t m : it->second) {
        bool is_usable_s0  = (machine_states[m] == S0 && !powering_down_machines.count(m));
        bool is_waking_up  = waking_machines.count(m) > 0;
        if (is_usable_s0 || is_waking_up)
            ++count;
    }
    return count;
}

// Find an active (S0, fully transitioned, not powering down) machine of the
// correct CPU type that has enough free memory to accept a new task.
//
// Two-pass approach (mirrors first-fit preference from the paper):
//   Pass 1 – prefer machines that already have a compatible VM; only task
//             memory is needed, saving VM_MEMORY_OVERHEAD.
//   Pass 2 – any machine with room for a brand-new VM + task.
//
// Returns MachineId_t(-1) when nothing fits.
MachineId_t Scheduler::findActiveMachine(CPUType_t cpu, VMType_t vm_type,
                                         unsigned task_mem) {
    auto it = machines_by_cpu.find(cpu);
    if (it == machines_by_cpu.end()) return MachineId_t(-1);

    MachineId_t best_with_vm    = MachineId_t(-1);
    MachineId_t best_without_vm = MachineId_t(-1);

    for (MachineId_t m : it->second) {
        // Must be fully active and stable
        if (machine_states[m]         != S0) continue;
        if (waking_machines.count(m))        continue;
        if (powering_down_machines.count(m)) continue;

        unsigned free = getFreeMemory(m);

        // Does this machine already have a compatible (non-migrating) VM?
        bool has_compat_vm = false;
        for (VMId_t vm : vms_on_machine[m]) {
            if (vm_types[vm] == vm_type && !migrating_vms.count(vm)) {
                has_compat_vm = true;
                break;
            }
        }

        if (has_compat_vm) {
            // Existing VM: only task memory is needed
            if (free >= task_mem && best_with_vm == MachineId_t(-1))
                best_with_vm = m;
        } else {
            // New VM: need task memory + VM overhead
            if (free >= task_mem + VM_MEMORY_OVERHEAD && best_without_vm == MachineId_t(-1))
                best_without_vm = m;
        }

        // Early exit: found the best possible option (existing VM with room)
        if (best_with_vm != MachineId_t(-1) && best_without_vm != MachineId_t(-1))
            break;
    }

    return (best_with_vm != MachineId_t(-1)) ? best_with_vm : best_without_vm;
}

// Find any machine of the given CPU type that is NOT currently active and NOT
// already waking up – i.e., a machine we can issue Machine_SetState(S0) to.
MachineId_t Scheduler::findSleepingMachine(CPUType_t cpu) {
    auto it = machines_by_cpu.find(cpu);
    if (it == machines_by_cpu.end()) return MachineId_t(-1);

    for (MachineId_t m : it->second) {
        bool is_sleeping = (machine_states[m] != S0);        // genuinely asleep
        bool already_waking     = waking_machines.count(m);
        bool already_powering_d = powering_down_machines.count(m);

        // Also accept a machine that is "powering down" (still S0 in machine_states)
        // but whose new state will be S3 – we can reverse-wake it.
        if (!is_sleeping && !already_powering_d) continue;  // it IS active
        if (already_waking) continue;                        // already being woken

        return m;  // valid sleeping (or mid-shutdown) machine
    }
    return MachineId_t(-1);
}

// ── VM helpers ────────────────────────────────────────────────────────────────

// Return an existing non-migrating VM of the right type on `machine`, or
// create + attach a new one if none exists.
VMId_t Scheduler::findOrCreateVM(MachineId_t machine, VMType_t vm_type, CPUType_t cpu) {
    for (VMId_t vm : vms_on_machine[machine]) {
        if (vm_types[vm] == vm_type && !migrating_vms.count(vm))
            return vm;
    }
    // Create and attach a new VM
    VMId_t vm = VM_Create(vm_type, cpu);
    VM_Attach(vm, machine);
    vms_on_machine[machine].push_back(vm);
    vm_types[vm] = vm_type;
    return vm;
}

// Remove `vm_id` from the vms_on_machine vector for `machine`.
void Scheduler::removeVMFromMachine(VMId_t vm_id, MachineId_t machine) {
    auto& vec = vms_on_machine[machine];
    vec.erase(std::remove(vec.begin(), vec.end(), vm_id), vec.end());
}

// ── Task placement ────────────────────────────────────────────────────────────

// Place `task_id` on `machine`.  Finds / creates a compatible VM, calls
// VM_AddTask, and updates both reverse-lookup maps.
void Scheduler::assignTask(MachineId_t machine, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    VMId_t vm = findOrCreateVM(machine, info.required_vm, info.required_cpu);
    VM_AddTask(vm, task_id, info.priority);
    task_to_vm[task_id]      = vm;
    task_to_machine[task_id] = machine;
}

// Called when a machine finishes waking up (StateChangeComplete → WakeupComplete).
// Tries to schedule every task that was queued while the machine was sleeping.
// Tasks that still don't fit (shouldn't normally happen) remain in pending_tasks.
void Scheduler::dispatchPendingTasks(MachineId_t machine) {
    auto& tasks = pending_tasks[machine];
    std::vector<TaskId_t> still_pending;

    for (TaskId_t task_id : tasks) {
        TaskInfo_t info = GetTaskInfo(task_id);
        if (info.completed) continue;   // guard: skip tasks that somehow finished

        // Determine memory needed, accounting for whether a compatible VM exists.
        bool has_compat_vm = false;
        for (VMId_t vm : vms_on_machine[machine]) {
            if (vm_types[vm] == info.required_vm && !migrating_vms.count(vm)) {
                has_compat_vm = true;
                break;
            }
        }
        unsigned needed = info.required_memory + (has_compat_vm ? 0u : VM_MEMORY_OVERHEAD);

        if (getFreeMemory(machine) >= needed) {
            assignTask(machine, task_id);
        } else {
            still_pending.push_back(task_id);
        }
    }
    tasks = std::move(still_pending);
}

// ── Energy management ─────────────────────────────────────────────────────────

// Power down `machine` to S3 (suspend), but only if it is safe to do so.
// Safety checks: must be S0, not already in transition, and no VMs / tasks.
void Scheduler::powerDownMachine(MachineId_t machine) {
    if (machine_states[machine]         != S0) return;   // already asleep
    if (waking_machines.count(machine))        return;   // mid-wakeup
    if (powering_down_machines.count(machine)) return;   // already shutting down

    // Double-check there are no tasks left (Machine_GetInfo is ground truth)
    MachineInfo_t info = Machine_GetInfo(machine);
    if (info.active_tasks > 0) return;

    // Shut down any lingering empty VMs first
    auto vms_copy = vms_on_machine[machine];
    for (VMId_t vm : vms_copy) {
        if (!migrating_vms.count(vm)) {
            VM_Shutdown(vm);
            vm_types.erase(vm);
        }
    }
    vms_on_machine[machine].clear();

    powering_down_machines.insert(machine);
    Machine_SetState(machine, S3);
    // machine_states[machine] remains S0 until StateChangeComplete fires
    SimOutput("Scheduler: powering down machine " + std::to_string(machine), 4);
}

// Walk every active machine and power down those that are idle, preserving at
// least one hot machine per CPU type as a hot spare.
void Scheduler::shutdownIdleMachines() {
    for (auto& [cpu, machines] : machines_by_cpu) {
        if (countActiveMachines(cpu) <= 1) continue;   // preserve the last one

        for (MachineId_t m : machines) {
            if (countActiveMachines(cpu) <= 1) break;  // re-check after each shutdown

            if (machine_states[m]         != S0) continue;
            if (waking_machines.count(m))        continue;
            if (powering_down_machines.count(m)) continue;

            MachineInfo_t info = Machine_GetInfo(m);
            if (info.active_tasks > 0) continue;

            // Any migrating VM going TO this machine? Skip it.
            bool has_migrating_vm = false;
            for (VMId_t vm : vms_on_machine[m]) {
                if (migrating_vms.count(vm)) { has_migrating_vm = true; break; }
            }
            if (has_migrating_vm) continue;

            // Any VM with active tasks? (should be covered by active_tasks check,
            // but double-check via VM_GetInfo to be safe)
            bool vm_has_tasks = false;
            for (VMId_t vm : vms_on_machine[m]) {
                VMInfo_t vi = VM_GetInfo(vm);
                if (!vi.active_tasks.empty()) { vm_has_tasks = true; break; }
            }
            if (vm_has_tasks) continue;

            // Safe to power down
            powerDownMachine(m);
        }
    }
}

// Sercon-style consolidation (Algorithm 3 from the paper, adapted to simulator).
//
// For each CPU family:
//   1. Sort active machines ascending by memory utilisation.
//   2. Take the least loaded machine (src) if util < 20 % (underload threshold).
//   3. Plan destinations for ALL of src's VMs (all-or-nothing, tracking
//      provisional extra memory so we don't over-commit destinations).
//   4. If the plan is feasible, execute: VM_Migrate each VM and update all maps.
//   5. Leave src empty; shutdownIdleMachines() will power it down next pass.
void Scheduler::tryConsolidate() {
    for (auto& [cpu, machines] : machines_by_cpu) {

        // ── Collect and sort active machines ─────────────────────────────────
        std::vector<MachineId_t> active;
        for (MachineId_t m : machines) {
            if (machine_states[m]         != S0) continue;
            if (waking_machines.count(m))        continue;
            if (powering_down_machines.count(m)) continue;
            active.push_back(m);
        }
        if (active.size() <= 1) continue;   // nothing to consolidate into

        // Sort ascending by memory utilisation (index 0 = least loaded)
        std::sort(active.begin(), active.end(), [&](MachineId_t a, MachineId_t b) {
            MachineInfo_t ia = Machine_GetInfo(a);
            MachineInfo_t ib = Machine_GetInfo(b);
            float ua = ia.memory_size ? float(ia.memory_used) / ia.memory_size : 0.f;
            float ub = ib.memory_size ? float(ib.memory_used) / ib.memory_size : 0.f;
            return ua < ub;
        });

        MachineId_t src = active[0];    // candidate to evacuate

        // ── Underload guard ───────────────────────────────────────────────────
        MachineInfo_t src_info = Machine_GetInfo(src);
        float src_util = src_info.memory_size
                         ? float(src_info.memory_used) / src_info.memory_size
                         : 0.f;
        if (src_util >= 0.2f) continue;     // not underloaded enough

        // ── Bail if any VM on src is mid-migration (can't plan reliably) ─────
        bool any_migrating = false;
        for (VMId_t vm : vms_on_machine[src]) {
            if (migrating_vms.count(vm)) { any_migrating = true; break; }
        }
        if (any_migrating) continue;

        const auto& src_vms = vms_on_machine[src];
        if (src_vms.empty()) continue;   // nothing to move; shutdownIdleMachines handles it

        // ── Plan destinations (all-or-nothing) ───────────────────────────────
        // provisional[m] tracks how much extra memory we've tentatively committed
        // to machine m in THIS planning pass.
        std::unordered_map<MachineId_t, unsigned, EnumHash> provisional;
        std::vector<std::pair<VMId_t, MachineId_t>>         plan;
        bool can_migrate_all = true;

        for (VMId_t vm : src_vms) {
            unsigned vm_mem = getVMMemory(vm);
            MachineId_t dst = MachineId_t(-1);

            // Prefer packing onto more-loaded machines (higher index = more loaded)
            for (int i = int(active.size()) - 1; i >= 1; --i) {
                MachineId_t cand = active[i];
                MachineInfo_t ci = Machine_GetInfo(cand);
                unsigned already_reserved = provisional.count(cand) ? provisional[cand] : 0;
                unsigned free = (ci.memory_size > ci.memory_used + already_reserved)
                                ? ci.memory_size - ci.memory_used - already_reserved
                                : 0u;
                if (free >= vm_mem) { dst = cand; break; }
            }

            if (dst == MachineId_t(-1)) {
                can_migrate_all = false;
                break;
            }
            provisional[dst] += vm_mem;
            plan.emplace_back(vm, dst);
        }

        if (!can_migrate_all) continue;     // all-or-nothing: skip this machine

        // ── Execute the plan ─────────────────────────────────────────────────
        // Work from a copy so we're not mutating vms_on_machine[src] while we
        // iterate plan (plan indexes into vms_on_machine[src] indirectly via vm ids).
        for (auto& [vm, dst] : plan) {
            migrating_vms.insert(vm);
            VM_Migrate(vm, dst);

            // Immediately update bookkeeping so subsequent lookups are consistent.
            removeVMFromMachine(vm, src);
            vms_on_machine[dst].push_back(vm);

            // Update reverse-lookup for every task inside the migrating VM
            VMInfo_t vinfo = VM_GetInfo(vm);
            for (TaskId_t t : vinfo.active_tasks)
                task_to_machine[t] = dst;

            SimOutput("Scheduler::tryConsolidate: migrating VM "
                      + std::to_string(vm) + " from " + std::to_string(src)
                      + " to " + std::to_string(dst), 4);
        }
        // src is now empty; shutdownIdleMachines() will power it down
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public scheduler methods
// ═══════════════════════════════════════════════════════════════════════════════

void Scheduler::Init() {
    unsigned total = Machine_GetTotal();

    machines_by_cpu.reserve(4);
    machine_states.reserve(total);
    vms_on_machine.reserve(total);
    pending_tasks.reserve(total);
    vm_types.reserve(total * 2);
    task_to_vm.reserve(128);
    task_to_machine.reserve(128);
    waking_machines.reserve(total / 2);
    powering_down_machines.reserve(total / 2);
    migrating_vms.reserve(16);

    for (unsigned i = 0; i < total; ++i) {
        MachineId_t cur = MachineId_t(i);
        CPUType_t   cpu = Machine_GetCPUType(cur);

        machines_by_cpu[cpu].push_back(cur);
        machine_states[cur] = S0;           // simulator starts all machines active
        vms_on_machine[cur] = {};
        pending_tasks[cur]  = {};
    }
    // All machines are active at start.  PeriodicCheck will power down idle
    // ones once it runs for the first time.
    SimOutput("Scheduler::Init(): " + std::to_string(total) + " machines registered", 4);
}

// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info    = GetTaskInfo(task_id);
    CPUType_t  cpu     = info.required_cpu;
    VMType_t   vm_type = info.required_vm;
    unsigned   mem     = info.required_memory;

    SimOutput("Scheduler::NewTask: task " + std::to_string(task_id)
              + " mem=" + std::to_string(mem), 4);

    // ── Fast path: find an already-active machine with enough room ────────────
    MachineId_t target = findActiveMachine(cpu, vm_type, mem);
    if (target != MachineId_t(-1)) {
        assignTask(target, task_id);
        return;
    }

    // ── Slow path: wake a sleeping machine and queue the task ─────────────────
    MachineId_t sleeping = findSleepingMachine(cpu);
    if (sleeping != MachineId_t(-1)) {
        waking_machines.insert(sleeping);
        Machine_SetState(sleeping, S0);
        // machine_states[sleeping] is NOT changed here; WakeupComplete sets it.
        pending_tasks[sleeping].push_back(task_id);
        SimOutput("Scheduler::NewTask: waking machine " + std::to_string(sleeping)
                  + " for task " + std::to_string(task_id), 4);
        return;
    }

    // ── Last resort: all machines of this CPU type are active or waking ───────
    // Queue onto whichever waking machine is most likely to have room first.
    auto it = machines_by_cpu.find(cpu);
    if (it != machines_by_cpu.end()) {
        for (MachineId_t m : it->second) {
            if (waking_machines.count(m)) {
                pending_tasks[m].push_back(task_id);
                SimOutput("Scheduler::NewTask: queued task " + std::to_string(task_id)
                          + " on waking machine " + std::to_string(m), 4);
                return;
            }
        }
    }
    // If we reach here, something is very wrong (no machine of the required
    // CPU type exists at all).  Log and move on.
    SimOutput("Scheduler::NewTask: ERROR – no machine for task "
              + std::to_string(task_id), 0);
}

// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    SimOutput("Scheduler::TaskComplete: task " + std::to_string(task_id)
              + " at " + std::to_string(now), 4);

    auto vm_it = task_to_vm.find(task_id);
    if (vm_it == task_to_vm.end()) {
        // Task was never assigned to a VM (e.g. it was still pending).
        // Remove it from any pending list it might be in.
        for (auto& [m, tasks] : pending_tasks) {
            tasks.erase(std::remove(tasks.begin(), tasks.end(), task_id), tasks.end());
        }
        return;
    }

    MachineId_t machine_id = task_to_machine[task_id];

    task_to_vm.erase(task_id);
    task_to_machine.erase(task_id);

    // ── Opportunistic performance throttle ────────────────────────────────────
    // If the machine now has no active tasks, drop all cores to P2 (half speed)
    // to save energy while idle.  They will be boosted back to P0 on SLA warning.
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.active_tasks == 0) {
        for (unsigned core = 0; core < info.num_cpus; ++core)
            Machine_SetCorePerformance(machine_id, core, P2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
    SimOutput("Scheduler::MigrationComplete: VM " + std::to_string(vm_id)
              + " at " + std::to_string(time), 4);
}

// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::PeriodicCheck(Time_t now) {
    // 1. Try to consolidate underloaded machines (evacuate → power down).
    tryConsolidate();
    // 2. Power down any machines that became idle since the last check.
    shutdownIdleMachines();
}

// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::Shutdown(Time_t time) {
    // Graceful teardown: shut down every VM we know about.
    for (auto& [machine, vms] : vms_on_machine) {
        for (VMId_t vm : vms) {
            VM_Shutdown(vm);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Callbacks forwarded from the global interface
// ─────────────────────────────────────────────────────────────────────────────

void Scheduler::WakeupComplete(Time_t time, MachineId_t machine_id) {
    if (waking_machines.count(machine_id)) {
        // Machine finished transitioning to S0
        waking_machines.erase(machine_id);
        machine_states[machine_id] = S0;
        SimOutput("Scheduler: machine " + std::to_string(machine_id) + " is now S0", 4);

        // Restore cores to a reasonable performance level (P1 – 3/4 speed)
        MachineInfo_t info = Machine_GetInfo(machine_id);
        for (unsigned core = 0; core < info.num_cpus; ++core)
            Machine_SetCorePerformance(machine_id, core, P1);

        dispatchPendingTasks(machine_id);
        return;
    }

    if (powering_down_machines.count(machine_id)) {
        // Machine finished transitioning to S3
        powering_down_machines.erase(machine_id);
        machine_states[machine_id] = S3;
        SimOutput("Scheduler: machine " + std::to_string(machine_id) + " is now S3", 4);
        return;
    }
    // Unexpected state-change completion – ignore silently
}

// ─────────────────────────────────────────────────────────────────────────────

// Called when the simulator detects memory overcommitment on `machine_id`.
// Strategy (overload relocation): migrate the single heaviest VM to whichever
// machine has the most headroom.  If no active machine can absorb it, wake a
// sleeping one (the memory warning will fire again once the machine is alive).
void Scheduler::HandleMemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("Scheduler::HandleMemoryWarning: machine "
              + std::to_string(machine_id), 0);

    MachineInfo_t minfo = Machine_GetInfo(machine_id);
    CPUType_t cpu = minfo.cpu;

    // Find the heaviest non-migrating VM on this machine
    VMId_t   heaviest_vm  = VMId_t(-1);
    unsigned heaviest_mem = 0;

    for (VMId_t vm : vms_on_machine[machine_id]) {
        if (migrating_vms.count(vm)) continue;
        unsigned vm_mem = getVMMemory(vm);
        if (vm_mem > heaviest_mem) {
            heaviest_mem = vm_mem;
            heaviest_vm  = vm;
        }
    }
    if (heaviest_vm == VMId_t(-1)) return;  // everything already migrating

    // Find the destination with the most free memory
    MachineId_t best_dst  = MachineId_t(-1);
    unsigned    best_free = 0;

    auto it = machines_by_cpu.find(cpu);
    if (it != machines_by_cpu.end()) {
        for (MachineId_t dst : it->second) {
            if (dst == machine_id)            continue;
            if (machine_states[dst] != S0)    continue;
            if (waking_machines.count(dst))   continue;
            if (powering_down_machines.count(dst)) continue;

            unsigned free = getFreeMemory(dst);
            if (free >= heaviest_mem && free > best_free) {
                best_free = free;
                best_dst  = dst;
            }
        }
    }

    if (best_dst != MachineId_t(-1)) {
        // Migrate immediately
        migrating_vms.insert(heaviest_vm);
        VM_Migrate(heaviest_vm, best_dst);
        removeVMFromMachine(heaviest_vm, machine_id);
        vms_on_machine[best_dst].push_back(heaviest_vm);

        VMInfo_t vinfo = VM_GetInfo(heaviest_vm);
        for (TaskId_t t : vinfo.active_tasks)
            task_to_machine[t] = best_dst;

        SimOutput("Scheduler::HandleMemoryWarning: migrated VM "
                  + std::to_string(heaviest_vm) + " to machine "
                  + std::to_string(best_dst), 4);
    } else {
        // No active destination – wake a sleeping machine.
        // The memory warning will re-fire; by then we can migrate.
        MachineId_t sleeping = findSleepingMachine(cpu);
        if (sleeping != MachineId_t(-1)) {
            waking_machines.insert(sleeping);
            Machine_SetState(sleeping, S0);
            SimOutput("Scheduler::HandleMemoryWarning: waking machine "
                      + std::to_string(sleeping) + " to relieve overload", 4);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

// Boost all cores on the machine running `task_id` to P0 (full speed).
// Called when the simulator detects an imminent SLA violation.
void Scheduler::HandleSLAWarning(Time_t time, TaskId_t task_id) {
    auto it = task_to_machine.find(task_id);
    if (it == task_to_machine.end()) return;

    MachineId_t machine_id = it->second;
    MachineInfo_t info     = Machine_GetInfo(machine_id);

    SimOutput("Scheduler::HandleSLAWarning: boosting machine "
              + std::to_string(machine_id) + " for task "
              + std::to_string(task_id), 4);

    for (unsigned core = 0; core < info.num_cpus; ++core)
        Machine_SetCorePerformance(machine_id, core, P0);
}


// ═══════════════════════════════════════════════════════════════════════════════
// Global interface – single static Scheduler instance
// ═══════════════════════════════════════════════════════════════════════════════

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): task " + std::to_string(task_id)
              + " at " + std::to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): task " + std::to_string(task_id)
              + " at " + std::to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): machine " + std::to_string(machine_id)
              + " at " + std::to_string(time), 0);
    Scheduler.HandleMemoryWarning(time, machine_id);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    printf("SLA violation report\n");
    printf("SLA0: %f%%\n", GetSLAReport(SLA0));
    printf("SLA1: %f%%\n", GetSLAReport(SLA1));
    printf("SLA2: %f%%\n", GetSLAReport(SLA2));
    printf("Total Energy %fKW-Hour\n", Machine_GetClusterEnergy());
    printf("Simulation run finished in %f seconds\n", double(time) / 1000000);
    SimOutput("SimulationComplete(): Simulation finished at time "
              + std::to_string(time), 4);
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    Scheduler.HandleSLAWarning(time, task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Handles BOTH wakeup completions and power-down completions.
    Scheduler.WakeupComplete(time, machine_id);
}