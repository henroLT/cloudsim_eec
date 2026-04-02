//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"

void Scheduler::Init() {
    unsigned total = Machine_GetTotal();
    
    // Step 1: Build CPU → machine map
    for (unsigned i = 0; i < total; ++i) {
        MachineId_t mid = MachineId_t(i);
        CPUType_t cpu = Machine_GetCPUType(mid);
        machines_by_cpu[cpu].push_back(mid);
        machine_states[mid] = S0;  // assume all start awake
    }

    // Step 2: Keep first 2 per CPU type awake, sleep the rest
    for (auto& [cpu, machines] : machines_by_cpu) {
        printf("[Init] CPU type %d has %zu machines\n", cpu, machines.size());
        
        for (unsigned i = 0; i < machines.size(); ++i) {
            if (i < 2) {
                printf("[Init] Keeping machine %u awake\n", machines[i]);
                // already S0, don't touch
            } else {
                printf("[Init] Sleeping machine %u\n", machines[i]);
                Machine_SetState(machines[i], S1);
                machine_states[machines[i]] = S1;
            }
        }
    }
}void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {}
void Scheduler::NewTask(Time_t now, TaskId_t task_id) {}
void Scheduler::PeriodicCheck(Time_t now) {}
void Scheduler::Shutdown(Time_t time) {}
void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {}
void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id) {}
void Scheduler::SLAWarning(Time_t time, TaskId_t task_id) {}
void Scheduler::MemoryWarning(Time_t time, MachineId_t machine_id) {}

std::optional<MachineId_t> Scheduler::getAvailableMachine(CPUType_t cpu, bool needs_gpu) { return std::nullopt; }
VMId_t Scheduler::getOrCreateVM(MachineId_t machine, VMType_t vm_type, CPUType_t cpu) { return VMId_t(-1); }
void Scheduler::wakeMachine(MachineId_t machine) {}
Priority_t Scheduler::getPriority(SLAType_t sla) { return LOW_PRIORITY; }


static Scheduler GlobalScheduler;

void InitScheduler() { GlobalScheduler.Init(); }
void HandleNewTask(Time_t time, TaskId_t task_id) { GlobalScheduler.NewTask(time, task_id); }
void HandleTaskCompletion(Time_t time, TaskId_t task_id) { GlobalScheduler.TaskComplete(time, task_id); }
void MemoryWarning(Time_t time, MachineId_t machine_id) { GlobalScheduler.MemoryWarning(time, machine_id); }
void MigrationDone(Time_t time, VMId_t vm_id) { GlobalScheduler.MigrationComplete(time, vm_id); }
void SchedulerCheck(Time_t time) { GlobalScheduler.PeriodicCheck(time); }
void SLAWarning(Time_t time, TaskId_t task_id) { GlobalScheduler.SLAWarning(time, task_id); }
void StateChangeComplete(Time_t time, MachineId_t machine_id) { GlobalScheduler.StateChangeComplete(time, machine_id); }

void SimulationComplete(Time_t time) {
    printf("SLA violation report\n");
    printf("SLA0: %f%%\n", GetSLAReport(SLA0));
    printf("SLA1: %f%%\n", GetSLAReport(SLA1));
    printf("SLA2: %f%%\n", GetSLAReport(SLA2));
    printf("Total Energy %fKW-Hour\n", Machine_GetClusterEnergy());
    printf("Simulation run finished in %f seconds\n", double(time)/1000000);
    GlobalScheduler.Shutdown(time);
}

