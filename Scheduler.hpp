#pragma once

#include "Interfaces.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Scheduler {
public:
    Scheduler() {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    void StateChangeComplete(Time_t time, MachineId_t machine_id);

private:
    std::unordered_map<CPUType_t,   std::vector<MachineId_t>> machines_by_cpu;
    std::unordered_map<MachineId_t, std::vector<VMId_t>>      vms_on_machine;
    std::unordered_map<VMId_t,      VMType_t>                 vm_types;
    std::unordered_map<MachineId_t, std::vector<TaskId_t>>    pending_tasks;
    
    std::unordered_map<MachineId_t, unsigned>                 pending_memory; 
    
    std::unordered_set<MachineId_t>                           waking_machines;
    std::unordered_set<MachineId_t>                           powering_down;
    std::unordered_map<MachineId_t, Time_t>                   empty_since;
    std::unordered_set<MachineId_t>                           sleeping_machines;
    std::unordered_map<VMId_t, std::pair<MachineId_t, MachineId_t>> migrating_vms;

    Time_t last_consolidation = 0;

    unsigned VMMemFootprint(VMId_t vm) const;
    bool     HasInboundMigration(MachineId_t m) const;
    bool     UnderloadRelocate(MachineId_t src, Time_t now);
    void     Consolidate(Time_t now);
};