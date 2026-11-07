// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

struct LiveNoticeLayout {
    UiRect button, card, close, body, accept;
    explicit LiveNoticeLayout(const UiLayout& layout) {
        button = {layout.width - 268, layout.height - 63, 44, 44};
        card = {layout.width - 544, layout.footer - 360, 514, 348};
        close = {card.x + card.w - 48, card.y + 13, 28, 28};
        body = {card.x + 22, card.y + 112, card.w - 49, 155};
        accept = {card.x + card.w - 190, card.y + 298, 168, 32};
    }
};

inline UiHit LiveNoticeHit(const live_notice::Panel& panel, const UiLayout& layout, float x, float y) {
    if (!panel.Visible()) return {};
    const LiveNoticeLayout l(layout);
    if (l.button.Contains(x, y)) return {UiAction::NoticeToggle};
    if (!panel.open) return {};
    if (l.close.Contains(x, y)) return {UiAction::NoticeClose};
    if (panel.NeedsAcknowledgment() && l.accept.Contains(x, y))
        return {UiAction::NoticeAcknowledge, panel.generation};
    if (l.card.Contains(x, y)) return {UiAction::NoticeBody};
    if (!layout.Play().Contains(x, y)) return {UiAction::NoticeClose};
    return {};
}

inline bool DispatchLiveNotice(live_notice::Panel& panel, UiHit hit) {
    if (!panel.Visible()) return false;
    switch (hit.action) {
    case UiAction::NoticeToggle:
        panel.open = !panel.open;
        panel.seen = true;
        return true;
    case UiAction::NoticeClose:
        panel.open = false;
        return true;
    case UiAction::NoticeBody:
        return true;
    case UiAction::NoticeAcknowledge:
        if (hit.index == panel.generation && panel.CanAcknowledge()) {
            panel.acknowledged = true;
            panel.open = false;
        }
        return true;
    default:
        return false;
    }
}

inline unsigned NoticeColor(const live_notice::Panel& panel) {
    using live_notice::Severity;
    if (!panel.status.playEnabled || panel.status.severity == Severity::Red) return 0xFF777E;
    if (panel.status.severity == Severity::Yellow) return 0xFFCF73;
    if (panel.status.severity == Severity::Blue) return 0x6DBEFF;
    return 0xB9C3D1;
}

inline const wchar_t* NoticeFooter(const live_notice::Panel& panel) {
    if (!panel.enabled) return nullptr;
    if (!panel.checked) return L"Checking live launcher status...";
    if (!panel.status.playEnabled) return L"Play is temporarily disabled. Open the status notice for details.";
    if (panel.NeedsAcknowledgment()) return L"Open the notice before playing.";
    if (panel.Offline()) return L"Offline mode. Current bug notices may be unavailable.";
    return nullptr;
}

inline void PaintLiveNotice(coop_manager_graphics::Renderer& renderer, live_notice::Panel& panel,
    const UiLayout& layout, UiHit hover) {
    if (!panel.Visible()) return;
    const LiveNoticeLayout l(layout);
    const unsigned accent = NoticeColor(panel);
    const float cx = l.button.x + l.button.w / 2;
    const float cy = l.button.y + l.button.h / 2;
    const auto now = GetTickCount64();
    if (panel.Pulsing(now)) {
        const auto phase = static_cast<float>(now % 1200) / 1200;
        renderer.Circle(cx, cy, 19 + phase * 3, accent, false, 1);
    }
    const bool hot = hover.action == UiAction::NoticeToggle || panel.open;
    renderer.Circle(cx, cy, 17, hot ? 0x343D48 : 0x222A33);
    renderer.Circle(cx, cy, 17, accent, false, 1.4f);
    renderer.Line(cx, cy - 8, cx, cy + 2, accent, 2.3f);
    renderer.Circle(cx, cy + 7, 1.4f, accent);
    if (!panel.open) return;

    using live_notice::Severity;
    const auto severity = panel.status.severity;
    const wchar_t* category = !panel.status.playEnabled ? L"PLAY DISABLED" :
        severity == Severity::Red ? L"CRITICAL NOTICE" :
        severity == Severity::Yellow ? L"IMPORTANT NOTICE" :
        severity == Severity::Blue ? L"ANNOUNCEMENT" :
        panel.Offline() ? L"OFFLINE STATUS" : L"LIVE STATUS";
    const wchar_t* title = !panel.status.title.empty() ? panel.status.title.c_str() :
        !panel.status.playEnabled ? L"Play is temporarily disabled" :
        panel.Offline() ? L"Live notices unavailable" : L"Checking for notices";
    const wchar_t* message = !panel.status.message.empty() ? panel.status.message.c_str() :
        !panel.status.playEnabled ? L"The developer has temporarily disabled launching the game. The launcher will update when Play is available again." :
        panel.Offline() ? L"You can play offline. The launcher cannot check for current bugs or announcements until it reconnects." :
        L"The launcher is checking for important notices before you play.";
    const auto& box = l.card;
    renderer.Box(box.x + 3, box.y + 5, box.w, box.h, 0x090B0E, 12);
    renderer.Box(box.x, box.y, box.w, box.h, 0x191E25, 12, 0x414C5A);
    renderer.Line(box.x + 16, box.y + 1, box.x + box.w - 16, box.y + 1, accent, 2);
    renderer.Line(cx - 8, box.y + box.h, cx, box.y + box.h + 8, accent, 1.5f);
    renderer.Line(cx, box.y + box.h + 8, cx + 8, box.y + box.h, accent, 1.5f);
    renderer.Text(category, box.x + 22, box.y + 20, box.w - 78, 18, 11, accent, UiFace::Bold);
    const float titleSize = renderer.Measure(title, 18, box.w - 44, 1000, UiFace::Bold, true).height > 60 ? 14.0f : 18.0f;
    renderer.Text(title, box.x + 22, box.y + 44, box.w - 44, 60, titleSize, 0xF0F3F8, UiFace::Bold, true);
    const auto& close = l.close;
    if (hover.action == UiAction::NoticeClose) renderer.Box(close.x, close.y, close.w, close.h, 0x333B46, 5);
    renderer.Line(close.x + 9, close.y + 9, close.x + 19, close.y + 19, 0xBAC5D2, 1.5f);
    renderer.Line(close.x + 19, close.y + 9, close.x + 9, close.y + 19, 0xBAC5D2, 1.5f);

    const auto& body = l.body;
    const float total = renderer.Measure(message, 14, body.w, 100000, UiFace::Regular, true).height;
    panel.maxScroll = (std::max)(0.0f, total - body.h);
    panel.scroll = (std::min)(panel.maxScroll, panel.scroll);
    renderer.Text(message, body.x, body.y, body.w, body.h, 14, 0xC8D2DF, UiFace::Regular, true, false, panel.scroll);
    if (panel.maxScroll > 0) {
        const float thumb = (std::max)(12.0f, body.h * body.h / total);
        const float x = box.x + box.w - 15;
        renderer.Box(x, body.y, 3, body.h, 0x333C49, 1.5f);
        renderer.Box(x, body.y + (body.h - thumb) * panel.scroll / panel.maxScroll, 3, thumb, accent, 1.5f);
    }
    renderer.Line(box.x + 22, box.y + 282, box.x + box.w - 22, box.y + 282, 0x323A47);
    panel.displayed = panel.generation;
    const bool canConfirm = panel.maxScroll - panel.scroll < 1;
    const bool acknowledge = panel.NeedsAcknowledgment();
    const wchar_t* detail = !canConfirm ? L"Scroll to read the full notice." :
        panel.Offline() ? L"Offline • last available status" : L"Updates automatically while open";
    renderer.Text(detail, box.x + 22, box.y + 304, acknowledge ? 295.0f : box.w - 44,
        20, 11, 0x9FADBF);
    if (acknowledge) {
        const auto& accept = l.accept;
        renderer.Box(accept.x, accept.y, accept.w, accept.h, canConfirm ? accent : 0x313845, 6);
        renderer.Text(severity == Severity::Red ? L"I acknowledge" : L"Mark as read",
            accept.x, accept.y, accept.w, accept.h, 13, canConfirm ? 0x151B24 : 0x8995A6,
            UiFace::Bold, false, true);
    }
}
