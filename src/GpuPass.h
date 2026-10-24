#pragma once

#include "Globals.h"

#include <string>

// Names one GPU pass for the internal profiler and for RenderDoc/PIX annotations.
struct ScopedGpuPass
{
	explicit ScopedGpuPass(const char* a_name)
	{
		if (globals::profiler) {
			globals::profiler->BeginPass(a_name);
			profilerActive = true;
		}

		auto* state = globals::state;
		if (state && state->frameAnnotations) {
			state->BeginPerfEvent(a_name);
			annotationOpen = true;
		}
	}

	~ScopedGpuPass()
	{
		if (annotationOpen && globals::state)
			globals::state->EndPerfEvent();
		if (profilerActive && globals::profiler)
			globals::profiler->EndPass();
	}

	ScopedGpuPass(const ScopedGpuPass&) = delete;
	ScopedGpuPass& operator=(const ScopedGpuPass&) = delete;

private:
	bool profilerActive = false;
	bool annotationOpen = false;
};

#define CS_GPU_PASS(name) const ScopedGpuPass cs_gpu_pass_scope { name }
