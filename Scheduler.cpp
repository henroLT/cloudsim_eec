//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"

void Scheduler::Init() {
    unsigned total = Machine_GetTotal();
    machines_by_cpu.reserve(4);
    vms_on_machine.reserve(total);
    pending_tasks.reserve(total);
    vm_types.reserve(total * 2);
    waking_machines.reserve(total / 2);

    for (unsigned i = 0; i < total; ++i) {
        MachineId_t cur = MachineId_t(i);
        machines_by_cpu[Machine_GetCPUType(cur)].push_back(cur);
        empty_since[cur] = 0; 
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    const auto& candidate_machines = machines_by_cpu[info.required_cpu];

    MachineId_t selected_machine = 0;
    bool machine_found = false;
    bool needs_new_vm = false;
    VMId_t target_vm = 0;
    double min_load = 999999.0;

    double time_to_deadline = (double)(info.target_completion - now);
    double required_mips = (time_to_deadline > 0)? 
        (double)info.remaining_instructions / time_to_deadline : 0.0;

    for (MachineId_t m_id : candidate_machines) {
        MachineInfo_t m_info = Machine_GetInfo(m_id);
        
        // Skip if machine is not fully awake or is currently waking up
        if (m_info.s_state != S0 || waking_machines.count(m_id)) continue;
        if (info.gpu_capable && !m_info.gpus) continue;
        if (m_info.performance[0] < required_mips * 1.05) continue;

        bool found_existing_vm = false;
        VMId_t temp_vm = 0;
        for (VMId_t vm_id : vms_on_machine[m_id]) {
            if (vm_types[vm_id] == info.required_vm) {
                temp_vm = vm_id;
                found_existing_vm = true;
                break;
            }
        }

        unsigned memory_needed = info.required_memory + (found_existing_vm ? 0 : VM_MEMORY_OVERHEAD);
        if ((m_info.memory_size - m_info.memory_used) < memory_needed) continue;

        double total_mips = (double)m_info.num_cpus * m_info.performance[0];
        double current_load = (double)m_info.active_tasks / total_mips;
        if (m_info.active_tasks >= m_info.num_cpus) current_load += 100.0;

        if (current_load < min_load) {
            min_load = current_load;
            selected_machine = m_id;
            machine_found = true;
            needs_new_vm = !found_existing_vm;
            target_vm = temp_vm;
        }
    }

    if (!machine_found) {
        min_load = 999999.0;
        for (MachineId_t m_id : candidate_machines) {
            MachineInfo_t m_info = Machine_GetInfo(m_id);
            
            if (m_info.s_state != S0 || waking_machines.count(m_id)) continue;
            if (info.gpu_capable && !m_info.gpus) continue;

            bool found_existing_vm = false;
            VMId_t temp_vm = 0;
            for (VMId_t vm_id : vms_on_machine[m_id]) {
                if (vm_types[vm_id] == info.required_vm) {
                    temp_vm = vm_id;
                    found_existing_vm = true;
                    break;
                }
            }

            unsigned memory_needed = info.required_memory + (found_existing_vm ? 0 : VM_MEMORY_OVERHEAD);
            if ((m_info.memory_size - m_info.memory_used) < memory_needed) continue;

            double total_mips = (double)m_info.num_cpus * m_info.performance[0];
            double current_load = (double)m_info.active_tasks / total_mips;

            if (current_load < min_load) {
                min_load = current_load;
                selected_machine = m_id;
                machine_found = true;
                needs_new_vm = !found_existing_vm;
                target_vm = temp_vm;
            }
        }
    }

    // If we found an active machine, assign the task
    if (machine_found) {
        // Machine is receiving a task, so it is no longer empty
        empty_since.erase(selected_machine); 
        
        if (needs_new_vm) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, selected_machine);
            vms_on_machine[selected_machine].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }

        VM_AddTask(target_vm, task_id, info.priority);

        if (info.required_sla == SLA0 || info.required_sla == SLA1) {
            unsigned num_cores = Machine_GetInfo(selected_machine).num_cpus;
            for (unsigned i = 0; i < num_cores; ++i) {
                Machine_SetCorePerformance(selected_machine, i, P0);
            }
        }
        return;
    }

    for (MachineId_t m_id : candidate_machines) {
        MachineInfo_t m_info = Machine_GetInfo(m_id);
        
        // Find a machine that is NOT S0 and NOT currently waking up
        if (m_info.s_state == S0 || waking_machines.count(m_id)) continue;
        if (info.gpu_capable && !m_info.gpus) continue;
        if (m_info.memory_size < (info.required_memory + VM_MEMORY_OVERHEAD)) continue;

        // Wake it up
        Machine_SetState(m_id, S0);
        waking_machines.insert(m_id);
        sleeping_machines.erase(m_id);
        empty_since.erase(m_id);
        pending_tasks[m_id].push_back(task_id);
        return;
    }

    SimOutput("Scheduler::NewTask(): CRITICAL - No machine can accommodate task " + std::to_string(task_id), 0);
}

void Scheduler::PeriodicCheck(Time_t now) {
    auto it = empty_since.begin();
    while (it != empty_since.end()) {
        MachineId_t m_id = it->first;
        Time_t idle_start = it->second;
        
        // If empty for 30 sec sleep
        if (now - idle_start >= 30000000) { 
            Machine_SetState(m_id, S3);
            sleeping_machines.insert(m_id);
            it = empty_since.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::Shutdown(Time_t time) {
    for (auto& [machine, vms] : vms_on_machine) {
        for (auto vm : vms) {
            VM_Shutdown(vm);
        }
    }
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    for (auto& [m_id, vms] : vms_on_machine) {
        auto it = vms.begin();
        while (it != vms.end()) {
            VMInfo_t vm_info = VM_GetInfo(*it);
            if (vm_info.active_tasks.empty()) {
                VM_Shutdown(*it);
                vm_types.erase(*it);
                it = vms.erase(it);
            } else {
                ++it;
            }
        }
        
        // If all VMs are cleared, tick
        if (vms.empty() && !empty_since.count(m_id)) {
            MachineInfo_t m_info = Machine_GetInfo(m_id);
            if (m_info.s_state == S0) {
                empty_since[m_id] = now;
            }
        }
    }
}

void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id) {
    if (!waking_machines.count(machine_id)) return;

    waking_machines.erase(machine_id);

    // Now that the machine is awake, process its pending tasks
    for (TaskId_t task_id : pending_tasks[machine_id]) {
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
    }

    pending_tasks[machine_id].clear();
}

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + std::to_string(task_id) + " at time " + std::to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + std::to_string(task_id) + " completed at time " + std::to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at " + std::to_string(machine_id) + " was detected at time " + std::to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    Scheduler.PeriodicCheck(time); // Link the global check to the class method
}

void SimulationComplete(Time_t time) {
    printf("SLA violation report\n");
    printf("SLA0: %f%%\n", GetSLAReport(SLA0));
    printf("SLA1: %f%%\n", GetSLAReport(SLA1));
    printf("SLA2: %f%%\n", GetSLAReport(SLA2));
    printf("Total Energy %fKW-Hour\n", Machine_GetClusterEnergy());
    printf("Simulation run finished in %f seconds\n", double(time)/1000000);
    SimOutput("SimulationComplete(): Simulation finished at time " + std::to_string(time), 4);
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    Scheduler.StateChangeComplete(time, machine_id);
}