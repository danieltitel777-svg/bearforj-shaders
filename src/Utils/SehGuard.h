#pragma once

#include <Windows.h>
#include <memory>
#include <type_traits>

namespace Util
{
	/**
	 * @brief Runs a_fn(a_context) under SEH. False, plus the exception code, when it faults.
	 *        A fault skips destructors in a_fn, so that function must not own objects that need one.
	 */
	bool InvokeSehGuarded(void (*a_fn)(void*), void* a_context, DWORD* a_exceptionCode = nullptr) noexcept;

	template <class F>
	bool SehGuarded(F&& a_fn, DWORD* a_exceptionCode = nullptr) noexcept
	{
		return InvokeSehGuarded(
			[](void* a_ctx) { (*static_cast<std::remove_reference_t<F>*>(a_ctx))(); },
			std::addressof(a_fn), a_exceptionCode);
	}
}
