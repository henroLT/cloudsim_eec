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
    }

    // Sleep all but 2 machines per cpu for energy saving
    for (auto& [cpu, machines] : machines_by_cpu) {
        for (unsigned i = 2; i < machines.size(); ++i) {
            Machine_SetState(machines[i], MachineState_t::S4);  // can try S5 or S3
            machine_states[machines[i]] = MachineState_t::S4;
        }
    }
}


void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);


    // Decide to attach the task to an existing VM, 
    //      vm.AddTask(taskid, Priority_T priority); or
    // Create a new VM, attach the VM to a machine
    //      VM vm(type of the VM)
    //      vm.Attach(machine_id);
    //      vm.AddTask(taskid, Priority_t priority) or
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    //
    // Turn on a machine, migrate an existing VM from a loaded machine....
    //
    // Other possibilities as desired
    // Skeleton code, you need to change it according to your algorithm
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
    // Do any bookkeeping necessary for the data structures
    // Decide if a machine is to be turned off, slowed down, or VMs to be migrated according to your policy
    // This is an opportunity to make any adjustments to optimize performance/energy
    SimOutput("Scheduler::TaskComplete(): Task " + std::to_string(task_id) + " is complete at " + std::to_string(now), 4);
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
    // Called in response to an earlier request to change the state of a machine
}

