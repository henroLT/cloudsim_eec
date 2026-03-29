//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Interfaces.h"
#include <unordered_map>
#include <unordered_set>


class Scheduler {
public:
    Scheduler()                 {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);

private:

    struct EnumHash {
        template <typename T>
        size_t operator() (T t) const {
            return static_cast<size_t>(t);
        }
    };
    
    std::unordered_map<CPUType_t,   std::vector<MachineId_t>, EnumHash>     machines_by_cpu;
    std::unordered_map<MachineId_t, MachineState_t,           EnumHash>     machine_states;
    std::unordered_map<MachineId_t, std::vector<VMId_t>,      EnumHash>     vms_on_machine;
    std::unordered_map<VMId_t,      VMType_t,                 EnumHash>     vm_types;
    std::unordered_map<MachineId_t, std::vector<TaskId_t>,    EnumHash>     pending_tasks;

    std::unordered_set<MachineId_t>   waking_machines;
    std::unordered_set<VMId_t>        migrating_vms;

    MachineId_t getAvailableMachine(CPUType_t cpu);
    VMId_t      getOrCreateVM(MachineId_t machine, VMType_t vm_type, CPUType_t cpu);
    void        wakeMachine(MachineId_t machine);
};

