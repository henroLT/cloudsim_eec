//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#pragma once

#include "Interfaces.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

class Scheduler {
public:
    Scheduler() {}

    // ── Primary lifecycle / event handlers ────────────────────────────────────
    void Init();
    void NewTask(Time_t now, TaskId_t task_id);
    void TaskComplete(Time_t now, TaskId_t task_id);
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);

    // ── Wrappers forwarded from the global callbacks ───────────────────────────
    //    These must be public so the file-scope global functions can call them.
    void WakeupComplete(Time_t time, MachineId_t machine_id);
    void HandleMemoryWarning(Time_t time, MachineId_t machine_id);
    void HandleSLAWarning(Time_t time, TaskId_t task_id);

private:
    // ── Hash helper that works for both plain enums and unsigned typedefs ─────
    struct EnumHash {
        template <typename T>
        size_t operator()(T t) const { return static_cast<size_t>(t); }
    };

    // ── Core bookkeeping ──────────────────────────────────────────────────────

    // All machines grouped by CPU type (populated once in Init, never changes)
    std::unordered_map<CPUType_t, std::vector<MachineId_t>, EnumHash> machines_by_cpu;

    // Cached machine state (kept in-sync with every Machine_SetState call)
    std::unordered_map<MachineId_t, MachineState_t, EnumHash> machine_states;

    // VMs currently attached to each machine (updated on create / migrate / shutdown)
    std::unordered_map<MachineId_t, std::vector<VMId_t>, EnumHash> vms_on_machine;

    // VM type cache so we can find a compatible VM without calling VM_GetInfo in a loop
    std::unordered_map<VMId_t, VMType_t, EnumHash> vm_types;

    // Tasks queued for a machine that is still waking up
    std::unordered_map<MachineId_t, std::vector<TaskId_t>, EnumHash> pending_tasks;

    // Task → VM / machine reverse-lookup (needed in TaskComplete & SLAWarning)
    std::unordered_map<TaskId_t, VMId_t,      EnumHash> task_to_vm;
    std::unordered_map<TaskId_t, MachineId_t, EnumHash> task_to_machine;

    // ── Transient state (machines / VMs mid-transition) ───────────────────────

    // Machines that received Machine_SetState(S0) but whose StateChangeComplete
    // has not fired yet.  Their machine_states entry still reflects the old
    // sleep state so findActiveMachine skips them automatically.
    std::unordered_set<MachineId_t, EnumHash> waking_machines;

    // Machines that received Machine_SetState(S3) but whose StateChangeComplete
    // has not fired yet.  Their machine_states entry is still S0, so we must
    // explicitly exclude them from placement / consolidation.
    std::unordered_set<MachineId_t, EnumHash> powering_down_machines;

    // VMs whose VM_Migrate call is in-flight.  We exclude them from
    // consolidation planning and do not double-count their memory.
    std::unordered_set<VMId_t, EnumHash> migrating_vms;

    // ── Private helpers ───────────────────────────────────────────────────────

    // Memory
    unsigned    getFreeMemory(MachineId_t m);
    unsigned    getVMMemory(VMId_t vm_id);          // VM_MEMORY_OVERHEAD + all task memory

    // Machine selection
    unsigned    countActiveMachines(CPUType_t cpu); // S0 + waking (not powering down)
    MachineId_t findActiveMachine(CPUType_t cpu, VMType_t vm_type, unsigned task_mem);
    MachineId_t findSleepingMachine(CPUType_t cpu);

    // VM helpers
    VMId_t  findOrCreateVM(MachineId_t machine, VMType_t vm_type, CPUType_t cpu);

    // Task placement
    void assignTask(MachineId_t machine, TaskId_t task_id);
    void dispatchPendingTasks(MachineId_t machine);

    // Energy management
    void tryConsolidate();
    void shutdownIdleMachines();
    void powerDownMachine(MachineId_t machine);

    // Utility
    void removeVMFromMachine(VMId_t vm_id, MachineId_t machine);
};