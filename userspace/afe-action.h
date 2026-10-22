// SPDX-License-Identifier: MIT
#pragma once

#include "TsaAfeControl.h"
#include "hx-v4l2-uapi.h"

namespace himax {

inline hx_raw_action HardwareAction(Solvers::Tsa::AfeActionType action) {
    using Action = Solvers::Tsa::AfeActionType;
    switch (action) {
    case Action::ClearStatus: return HX_RAW_CLEAR_STATUS;
    case Action::ForceToFreqPoint: return HX_RAW_FORCE_FREQ;
    case Action::ForceToScanRate: return HX_RAW_FORCE_RATE;
    case Action::EnterIdle: return HX_RAW_ENTER_IDLE;
    case Action::StartCalibration: return HX_RAW_CALIBRATE;
    }
    return HX_RAW_RESET;  // Callers reject unknown upstream actions.
}

}  // namespace himax
