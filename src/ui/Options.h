#pragma once

// The Nexus Options panel — three tabs (General / Quickbar / Unlockable
// Emotes) plus the lifecycle hook for the Quickbar's close-on-Escape
// registration. Defined here because the Options checkbox is the only
// runtime mutator, but also called from AddonLoad's initial setup.

void AddonOptions();

// Register / deregister the QB's close-on-Escape hook to match the current
// g_Settings.QuickbarCloseOnEsc value. Idempotent — safe to call repeatedly.
void ApplyQbCloseOnEsc();
