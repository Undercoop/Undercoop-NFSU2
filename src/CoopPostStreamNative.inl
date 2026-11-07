// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Called only after the verified native palette/Stage7 completion callback.
// Fleet assets and the race roster have independent post-stream proofs. A
// reserved traffic slot may have no live car/resource in this event; that must
// not prevent the actual, loaded race competitors from sealing their proof.
void OnCoopFleetNativeStage7Completed() {
    if ((!gCoopFleetEnabled && !gCoopAIEnabled) || !IsCoopGameplayActive()) return;
    if (gCoopFleetEnabled) {
        coop_world::Identity identity{};
        const auto generation = CoopFleetLocalLoadToken();
        if (!CurrentCoopFleetIdentity(identity) || !generation ||
            !NotifyCoopFleetAssetsAfterStage7(identity, generation) ||
            !NotifyCoopFleetNativeLoadReady(identity, generation)) {
            LogMessage("[FLEET_ASSETS] Post-stream readiness refused; unmatched replicas remain suppressed.\r\n");
            // Do not manufacture Fleet readiness or touch missing resources.
            // SealCoopAIRosterAfterStreaming separately validates exact event,
            // loaded recipes/parts, two humans and the final reserved tail.
        }
    }
    if (gCoopAIEnabled && !SealCoopAIRosterAfterStreaming())
        LogMessage("[AI_ROSTER] Final loaded restart proof unavailable; current race retained.\r\n");
}
