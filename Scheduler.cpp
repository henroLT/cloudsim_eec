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
    machine_states.reserve(total);
    vms_on_machine.reserve(total);
    pending_tasks.reserve(total);
    vm_types.reserve(total * 2);          // baseline guess (~2 per machine)

    waking_machines.reserve(total / 2);   // shouldnt be that many waking at once
    migrating_vms.reserve(8);             // shouldnt have VMs migrating at once

    
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
}


void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    const auto& candidate_machines = machines_by_cpu[info.required_cpu];
    
    MachineId_t selected_machine = 0;
    bool machine_found = false;
    bool needs_new_vm = false;
    VMId_t target_vm = 0;
    double min_load = 999999.0; 

    // --- NEW: Calculate the exact physical speed required to meet the SLA ---
    // Time is in microseconds. MIPS = Instructions / Microseconds.
    double time_to_deadline = (double)(info.target_completion - now);
    double required_mips = 0.0;
    if (time_to_deadline > 0) {
        required_mips = (double)info.remaining_instructions / time_to_deadline;
    }

    // --- FIRST PASS: Find least loaded machine that mathematically fits the deadline ---
    for (MachineId_t m_id : candidate_machines) {
        MachineInfo_t m_info = Machine_GetInfo(m_id);
        
        if (m_info.s_state == S5 || waking_machines.count(m_id)) continue;
        if (info.gpu_capable && !m_info.gpus) continue;

        // THE DEADLINE FILTER: Skip machines that are physically too slow to meet the SLA
        // We add a 5% buffer (1.05) to account for slight OS/hypervisor scheduling delays
        if (m_info.performance[0] < required_mips * 1.05) continue;

        bool found_existing_vm = false;
        VMId_t temp_target_vm = 0;
        for (VMId_t vm_id : vms_on_machine[m_id]) {
            if (vm_types[vm_id] == info.required_vm) {
                temp_target_vm = vm_id;
                found_existing_vm = true;
                break;
            }
        }

        unsigned memory_needed = info.required_memory;
        if (!found_existing_vm) memory_needed += VM_MEMORY_OVERHEAD;

        if ((m_info.memory_size - m_info.memory_used) >= memory_needed) {
            
            double total_mips = (double)m_info.num_cpus * m_info.performance[0];
            double current_load = (double)m_info.active_tasks / total_mips;
            
            // OVERSUBSCRIPTION PENALTY: Avoid time-sharing cores if possible!
            // If tasks >= cores, they fight for time, destroying effective MIPS.
            if (m_info.active_tasks >= m_info.num_cpus) {
                current_load += 100.0; 
            }
            
            if (current_load < min_load) {
                min_load = current_load;
                selected_machine = m_id;
                machine_found = true;
                needs_new_vm = !found_existing_vm;
                target_vm = temp_target_vm;
            }
        }
    }

    // --- FALLBACK PASS: If no machine is fast enough, just find the least loaded ---
    // (This prevents the datacenter from completely dropping tasks during massive spikes)
    if (!machine_found) {
        min_load = 999999.0;
        for (MachineId_t m_id : candidate_machines) {
            MachineInfo_t m_info = Machine_GetInfo(m_id);
            if (m_info.s_state == S5 || waking_machines.count(m_id)) continue;
            if (info.gpu_capable && !m_info.gpus) continue;

            bool found_existing_vm = false;
            VMId_t temp_target_vm = 0;
            for (VMId_t vm_id : vms_on_machine[m_id]) {
                if (vm_types[vm_id] == info.required_vm) {
                    temp_target_vm = vm_id;
                    found_existing_vm = true;
                    break;
                }
            }

            unsigned memory_needed = info.required_memory;
            if (!found_existing_vm) memory_needed += VM_MEMORY_OVERHEAD;

            if ((m_info.memory_size - m_info.memory_used) >= memory_needed) {
                double total_mips = (double)m_info.num_cpus * m_info.performance[0];
                double current_load = (double)m_info.active_tasks / total_mips;

                if (current_load < min_load) {
                    min_load = current_load;
                    selected_machine = m_id;
                    machine_found = true;
                    needs_new_vm = !found_existing_vm;
                    target_vm = temp_target_vm;
                }
            }
        }
    }

    // --- ATTACH TASK ---
    if (machine_found) {
        if (needs_new_vm) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, selected_machine);
            vms_on_machine[selected_machine].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }
        
        // Reverted to native priority to prevent SLA1 starvation!
        VM_AddTask(target_vm, task_id, info.priority);
        
        // Keep the Turbo Boost to clear queues fast
        if (info.required_sla == SLA0 || info.required_sla == SLA1) {
            unsigned num_cores = Machine_GetInfo(selected_machine).num_cpus;
            for (unsigned i = 0; i < num_cores; ++i) {
                Machine_SetCorePerformance(selected_machine, i, P0);
            }
        }
        return;
    }

    // --- WAKE UP PASS ---
    for (MachineId_t m_id : candidate_machines) {
        MachineInfo_t m_info = Machine_GetInfo(m_id);
        
        if (m_info.s_state != S5 || waking_machines.count(m_id)) continue;
        if (info.gpu_capable && !m_info.gpus) continue;
        
        if (m_info.memory_size >= (info.required_memory + VM_MEMORY_OVERHEAD)) {
            Machine_SetState(m_id, S0);
            waking_machines.insert(m_id);
            pending_tasks[m_id].push_back(task_id);
            return;
        }
    }

    SimOutput("Scheduler::NewTask(): CRITICAL - No machine can accommodate task " + std::to_string(task_id), 0);
}

void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary
}

void Scheduler::Shutdown(Time_t time) {
    for (auto& [machine, vms] : vms_on_machine) {
        for (auto vm : vms) {
            VM_Shutdown(vm);
        }
    }
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // The simulator handles the internal task completion and CPU freeing.
    // Our job is to do bookkeeping. Since we are keeping machines at S0,
    // we only need to sweep for empty VMs to reclaim their memory overhead.
    
    for (auto& [m_id, vms] : vms_on_machine) {
        
        // Use an iterator to safely erase items from the vector while looping
        auto it = vms.begin();
        while (it != vms.end()) {
            VMId_t vm_id = *it;
            VMInfo_t vm_info = VM_GetInfo(vm_id);
            
            // If the VM has no running tasks, destroy it
            if (vm_info.active_tasks.empty()) {
                VM_Shutdown(vm_id);             // Tell simulator to kill the VM
                vm_types.erase(vm_id);          // Remove from our type tracker
                it = vms.erase(it);             // Remove from this machine's VM list
                
                SimOutput("Scheduler::TaskComplete(): Reclaimed empty VM " + 
                          std::to_string(vm_id) + " on machine " + std::to_string(m_id), 4);
            } else {
                ++it; // Move to next VM
            }
        }
    }
    
    SimOutput("Scheduler::TaskComplete(): Task " + std::to_string(task_id) + 
              " complete bookkeeping finished at " + std::to_string(now), 4);
}

void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // 1. Verify this machine was in our waking queue
    if (waking_machines.find(machine_id) == waking_machines.end()) {
        return; // State change wasn't a wake-up, or we didn't track it.
    }

    // 2. The machine is now fully awake (S0 state)
    waking_machines.erase(machine_id);

    // 3. Process all tasks that were waiting for this machine
    auto& tasks = pending_tasks[machine_id];
    
    for (TaskId_t task_id : tasks) {
        TaskInfo_t info = GetTaskInfo(task_id);
        
        VMId_t target_vm = 0;
        bool found_existing_vm = false;

        // Check if a previous task in this same queue already created the right VM
        for (VMId_t vm_id : vms_on_machine[machine_id]) {
            if (vm_types[vm_id] == info.required_vm) {
                target_vm = vm_id;
                found_existing_vm = true;
                break;
            }
        }

        // If not, create and attach a new VM
        if (!found_existing_vm) {
            target_vm = VM_Create(info.required_vm, info.required_cpu);
            VM_Attach(target_vm, machine_id);
            
            vms_on_machine[machine_id].push_back(target_vm);
            vm_types[target_vm] = info.required_vm;
        }

        // 4. Assign the task to the VM
        VM_AddTask(target_vm, task_id, info.priority);
        SimOutput("Scheduler::StateChangeComplete(): Assigned queued task " + std::to_string(task_id) + " to machine " + std::to_string(machine_id), 4);
    }

    // Clear the queue now that all pending tasks are assigned
    tasks.clear();
}

// Public interface below

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
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + std::to_string(machine_id) + " was detected at time " + std::to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    // This function is called periodically by the simulator, no specific event
    // SimOutput("SchedulerCheck(): SchedulerCheck() called at " + std::to_string(time), 4);
    // Scheduler.PeriodicCheck(time);
    // static unsigned counts = 0;
    // counts++;
    // if(counts == 10) {
    //     VM_Migrate(1, 9);
    // }
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

void SLAWarning(Time_t time, TaskId_t task_id) {
    
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    Scheduler.StateChangeComplete(time, machine_id);
}

