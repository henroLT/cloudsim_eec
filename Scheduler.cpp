//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//  Modified to use Round-Robin Scheduling
//

#include "Scheduler.hpp"
#include <string>

// ------------------------------------------------------------
// Scheduler Initialization
// ------------------------------------------------------------
void Scheduler::Init() {
    unsigned total = Machine_GetTotal();

    machines_by_cpu.reserve(4);
    machine_states.reserve(total);
    vms_on_machine.reserve(total);
    pending_tasks.reserve(total);
    vm_types.reserve(total * 2);

    waking_machines.reserve(total / 2);
    migrating_vms.reserve(8);

    for (unsigned i = 0; i < total; ++i) {
        MachineId_t cur = MachineId_t(i);
        CPUType_t cpu = Machine_GetCPUType(cur);

        machines_by_cpu[cpu].push_back(cur);
        machine_states[cur] = MachineState_t::S0;

        unsigned num_cores = Machine_GetInfo(cur).num_cpus;
        for (unsigned core = 0; core < num_cores; ++core) {
            Machine_SetCorePerformance(cur, core, P0);
        }
    }

    // Initialize round-robin pointer per CPU type
    for (auto& entry : machines_by_cpu) {
        rr_index[entry.first] = 0;
    }
}

// ------------------------------------------------------------
// Helper: Can this machine host this task?
// ------------------------------------------------------------
bool Scheduler::CanHostTask(MachineId_t m_id,
                            const TaskInfo_t& info,
                            VMId_t& target_vm,
                            bool& needs_new_vm) {
    MachineInfo_t m_info = Machine_GetInfo(m_id);

    // Skip sleeping or waking machines
    if (m_info.s_state == S5 || waking_machines.count(m_id)) {
        return false;
    }

    // Skip if task requires GPU and machine doesn't have one
    if (info.gpu_capable && !m_info.gpus) {
        return false;
    }

    target_vm = 0;
    needs_new_vm = true;

    // Try to reuse an existing VM of the correct type
    for (VMId_t vm_id : vms_on_machine[m_id]) {
        if (vm_types[vm_id] == info.required_vm) {
            target_vm = vm_id;
            needs_new_vm = false;
            break;
        }
    }

    unsigned free_memory = m_info.memory_size - m_info.memory_used;
    unsigned required_memory = info.required_memory;

    if (needs_new_vm) {
        required_memory += VM_MEMORY_OVERHEAD;
    }

    return free_memory >= required_memory;
}

// ------------------------------------------------------------
// Migration Complete
// ------------------------------------------------------------
void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    (void)time;
    migrating_vms.erase(vm_id);
}

// ------------------------------------------------------------
// New Task Arrival
// ------------------------------------------------------------
void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    (void)now;

    TaskInfo_t info = GetTaskInfo(task_id);
    auto& candidate_machines = machines_by_cpu[info.required_cpu];

    if (candidate_machines.empty()) {
        SimOutput("Scheduler::NewTask(): No candidate machines for task " +
                  std::to_string(task_id), 0);
        return;
    }

    size_t start_index = rr_index[info.required_cpu];
    size_t selected_index = start_index;

    bool machine_found = false;
    MachineId_t selected_machine = 0;
    VMId_t target_vm = 0;
    bool needs_new_vm = false;

    // -------------------------------
    // ROUND ROBIN SEARCH
    // -------------------------------
    do {
        MachineId_t m_id = candidate_machines[selected_index];

        if (CanHostTask(m_id, info, target_vm, needs_new_vm)) {
            machine_found = true;
            selected_machine = m_id;

            // Advance RR pointer for next task
            rr_index[info.required_cpu] =
                (selected_index + 1) % candidate_machines.size();

            break;
        }

        selected_index = (selected_index + 1) % candidate_machines.size();

    } while (selected_index != start_index);

    // -------------------------------
    // ASSIGN TASK IF MACHINE FOUND
    // -------------------------------
    if (machine_found) {
        if (needs_new_vm) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, selected_machine);

            vms_on_machine[selected_machine].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }

        VM_AddTask(target_vm, task_id, info.priority);

        SimOutput("Scheduler::NewTask(): Assigned task " +
                  std::to_string(task_id) + " to machine " +
                  std::to_string(selected_machine), 4);
        return;
    }

    // -------------------------------
    // WAKE-UP PASS
    // -------------------------------
    for (MachineId_t m_id : candidate_machines) {
        MachineInfo_t m_info = Machine_GetInfo(m_id);

        if (m_info.s_state != S5 || waking_machines.count(m_id)) continue;
        if (info.gpu_capable && !m_info.gpus) continue;

        unsigned required_memory = info.required_memory + VM_MEMORY_OVERHEAD;

        if (m_info.memory_size >= required_memory) {
            Machine_SetState(m_id, S0);
            waking_machines.insert(m_id);
            pending_tasks[m_id].push_back(task_id);

            SimOutput("Scheduler::NewTask(): Waking machine " +
                      std::to_string(m_id) + " for task " +
                      std::to_string(task_id), 4);
            return;
        }
    }

    SimOutput("Scheduler::NewTask(): CRITICAL - No machine can accommodate task " +
              std::to_string(task_id), 0);
}

// ------------------------------------------------------------
// Periodic Check
// ------------------------------------------------------------
void Scheduler::PeriodicCheck(Time_t now) {
    (void)now;
    // Optional future monitoring / consolidation logic
}

// ------------------------------------------------------------
// Shutdown
// ------------------------------------------------------------
void Scheduler::Shutdown(Time_t time) {
    (void)time;

    for (auto& pair : vms_on_machine) {
        auto& vms = pair.second;
        for (VMId_t vm_id : vms) {
            VM_Shutdown(vm_id);
        }
    }
}

// ------------------------------------------------------------
// Task Completion
// ------------------------------------------------------------
void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    (void)task_id;

    // Clean up empty VMs
    for (auto& pair : vms_on_machine) {
        MachineId_t m_id = pair.first;
        auto& vms = pair.second;

        auto it = vms.begin();
        while (it != vms.end()) {
            VMId_t vm_id = *it;
            VMInfo_t vm_info = VM_GetInfo(vm_id);

            if (vm_info.active_tasks.empty()) {
                VM_Shutdown(vm_id);
                vm_types.erase(vm_id);

                SimOutput("Scheduler::TaskComplete(): Reclaimed empty VM " +
                          std::to_string(vm_id) + " on machine " +
                          std::to_string(m_id), 4);

                it = vms.erase(it);
            } else {
                ++it;
            }
        }
    }

    SimOutput("Scheduler::TaskComplete(): Task bookkeeping finished at " +
              std::to_string(now), 4);
}

// ------------------------------------------------------------
// Machine Wake-Up Completion
// ------------------------------------------------------------
void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id) {
    (void)time;

    // Only handle machines we explicitly woke up
    if (waking_machines.find(machine_id) == waking_machines.end()) {
        return;
    }

    waking_machines.erase(machine_id);

    auto& tasks = pending_tasks[machine_id];

    for (TaskId_t task_id : tasks) {
        TaskInfo_t info = GetTaskInfo(task_id);

        VMId_t target_vm = 0;
        bool found_existing_vm = false;

        for (VMId_t vm_id : vms_on_machine[machine_id]) {
            if (vm_types[vm_id] == info.required_vm) {
                target_vm = vm_id;
                found_existing_vm = true;
                break;
            }
        }

        if (!found_existing_vm) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, machine_id);

            vms_on_machine[machine_id].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }

        VM_AddTask(target_vm, task_id, info.priority);

        SimOutput("Scheduler::StateChangeComplete(): Assigned queued task " +
                  std::to_string(task_id) + " to machine " +
                  std::to_string(machine_id), 4);
    }

    tasks.clear();
}

// ------------------------------------------------------------
// Public Interface
// ------------------------------------------------------------
static Scheduler SchedulerInstance;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    SchedulerInstance.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " +
              std::to_string(task_id) + " at time " +
              std::to_string(time), 4);
    SchedulerInstance.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " +
              std::to_string(task_id) + " completed at time " +
              std::to_string(time), 4);
    SchedulerInstance.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at machine " +
              std::to_string(machine_id) + " detected at time " +
              std::to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SchedulerInstance.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    // Optional periodic scheduler hook
    // SchedulerInstance.PeriodicCheck(time);
    (void)time;
}

void SimulationComplete(Time_t time) {
    printf("SLA violation report\n");
    printf("SLA0: %f%%\n", GetSLAReport(SLA0));
    printf("SLA1: %f%%\n", GetSLAReport(SLA1));
    printf("SLA2: %f%%\n", GetSLAReport(SLA2));
    printf("Total Energy %fKW-Hour\n", Machine_GetClusterEnergy());
    printf("Simulation run finished in %f seconds\n", double(time) / 1000000);

    SimOutput("SimulationComplete(): Simulation finished at time " +
              std::to_string(time), 4);

    SchedulerInstance.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    (void)time;
    (void)task_id;
    // Optional SLA reaction logic
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    SchedulerInstance.StateChangeComplete(time, machine_id);
}