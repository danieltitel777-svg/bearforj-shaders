#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Applies generation-checked CPU control-plane commands to GPU-owned persistent
// emitter state while preserving runtime-maintained fields across updates.
StructuredBuffer<SSPPEmitterCommand> EmitterCommands : register(t0);
RWStructuredBuffer<SSPPPersistentEmitter> EmitterStates : register(u0);
RWStructuredBuffer<uint> Counters : register(u7);

// Summary: Increments the invalid-emitter-command diagnostic counter.
// Args: None.
// Returns: None.
void RecordInvalidCommand()
{
	InterlockedAdd(
		Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_INVALID_COMMANDS],
		1u);
}

[numthreads(64, 1, 1)]
// Summary: Applies one create, update, or destroy command to an emitter slot.
// Args:
//   dispatchID: Command index in its x component.
// Returns: None.
void ApplyEmitterCommandsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint commandIndex = dispatchID.x;
	if (commandIndex >= SSPPEmitterCommandCount)
		return;

	SSPPEmitterCommand command = EmitterCommands[commandIndex];
	if (command.EmitterIndex >= SSPPMaxEmitters || command.Emitter.Generation == 0u) {
		RecordInvalidCommand();
		return;
	}

	SSPPPersistentEmitter current = EmitterStates[command.EmitterIndex];
	if (command.Operation == SSPP_EMITTER_COMMAND_CREATE) {
		if (current.State != SSPP_EMITTER_STATE_FREE) {
			RecordInvalidCommand();
			return;
		}
		SSPPPersistentEmitter created = command.Emitter;
		created.SpawnAccumulator = 0.0f;
		created.AliveCount = 0u;
		created.RequestOrdinal = 0u;
		created.State = SSPP_EMITTER_STATE_ACTIVE;
		EmitterStates[command.EmitterIndex] = created;
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_CREATES],
			1u);
		return;
	}

	if (current.State != SSPP_EMITTER_STATE_ACTIVE ||
		current.Generation != command.Emitter.Generation) {
		RecordInvalidCommand();
		return;
	}

	if (command.Operation == SSPP_EMITTER_COMMAND_UPDATE) {
		SSPPPersistentEmitter updated = command.Emitter;
		updated.SpawnAccumulator = current.SpawnAccumulator;
		updated.AliveCount = current.AliveCount;
		updated.Generation = current.Generation;
		updated.RequestOrdinal = current.RequestOrdinal;
		updated.State = SSPP_EMITTER_STATE_ACTIVE;
		EmitterStates[command.EmitterIndex] = updated;
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_UPDATES],
			1u);
	} else if (command.Operation == SSPP_EMITTER_COMMAND_DESTROY) {
		current.Flags = 0u;
		current.State = SSPP_EMITTER_STATE_FREE;
		current.AliveCount = 0u;
		EmitterStates[command.EmitterIndex] = current;
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DESTROYS],
			1u);
	} else {
		RecordInvalidCommand();
	}
}
