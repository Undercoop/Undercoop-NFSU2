// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

// Portable sender-timeline playback. The intentional 90--160 ms buffer trades
// roughly 100 ms of visual latency for smooth motion; it cannot hide long loss.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop_motion {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

inline bool IsRotation(const float* m) {
    if (!m) return false;
    for (int r = 0; r != 3; ++r) {
        float length = 0.0f;
        for (int c = 0; c != 3; ++c) {
            const float v = m[r * 4 + c];
            if (!std::isfinite(v)) return false;
            length += v * v;
        }
        if (std::fabs(length - 1.0f) > 0.12f) return false;
        for (int other = 0; other != r; ++other) {
            float dot = 0.0f;
            for (int c = 0; c != 3; ++c) dot += m[r * 4 + c] * m[other * 4 + c];
            if (std::fabs(dot) > 0.08f) return false;
        }
    }
    const float determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    return determinant > 0.85f && determinant < 1.15f;
}

inline bool ValidQuaternion(const float* q) {
    if (!q) return false;
    float length = 0.0f;
    for (int i = 0; i != 4; ++i) {
        if (!std::isfinite(q[i])) return false;
        length += q[i] * q[i];
    }
    return length >= 0.8f && length <= 1.2f;
}

inline void NormalizeQuaternion(float* q) {
    const float scale = 1.0f / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i != 4; ++i) q[i] *= scale;
}

inline bool QuaternionToMatrix(const float* input, float* m) {
    if (!m || !ValidQuaternion(input)) return false;
    float q[4]{input[0], input[1], input[2], input[3]};
    NormalizeQuaternion(q);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // Transpose of the conventional column-vector matrix: NFSU2 stores its
    // forward/right/up basis in rows. Do not reconstruct this from yaw alone.
    const float result[16]{
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
        0, 0, 0, 1};
    std::memcpy(m, result, sizeof(result));
    return true;
}

inline bool MatrixToQuaternion(const float* m, float* q) {
    if (!q || !IsRotation(m)) return false;
    const float trace = m[0] + m[5] + m[10];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[6] - m[9]) / s;
        q[1] = (m[8] - m[2]) / s;
        q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2.0f;
        q[3] = (m[6] - m[9]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[4]) / s;
        q[2] = (m[2] + m[8]) / s;
    } else if (m[5] > m[10]) {
        const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2.0f;
        q[3] = (m[8] - m[2]) / s;
        q[0] = (m[1] + m[4]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[6] + m[9]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2.0f;
        q[3] = (m[1] - m[4]) / s;
        q[0] = (m[2] + m[8]) / s;
        q[1] = (m[6] + m[9]) / s;
        q[2] = 0.25f * s;
    }
    NormalizeQuaternion(q);
    return ValidQuaternion(q);
}

inline void Slerp(const float* a, const float* b, float t, float* out) {
    float qa[4]{a[0], a[1], a[2], a[3]}, qb[4]{b[0], b[1], b[2], b[3]};
    NormalizeQuaternion(qa);
    NormalizeQuaternion(qb);
    float dot = 0.0f;
    for (int i = 0; i != 4; ++i) dot += qa[i] * qb[i];
    if (dot < 0.0f) {
        for (float& value : qb) value = -value;
        dot = -dot;
    }
    dot = std::clamp(dot, 0.0f, 1.0f);
    if (dot >= 1.0f || (dot > 0.9995f && t >= 0.0f && t <= 1.0f)) {
        for (int i = 0; i != 4; ++i) out[i] = qa[i] + (qb[i] - qa[i]) * t;
    } else {
        const float theta = std::acos(dot), divisor = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / divisor, wb = std::sin(t * theta) / divisor;
        for (int i = 0; i != 4; ++i) out[i] = wa * qa[i] + wb * qb[i];
    }
    NormalizeQuaternion(out);
}

inline float WrapPhase(float phase) {
    float value = std::fmod(phase, kTau);
    return value < 0.0f ? value + kTau : value;
}

inline bool WheelQuaternion(const float* rotation, float* q) {
    float m[16]{};
    m[15] = 1.0f;
    for (int r = 0; r != 3; ++r)
        for (int c = 0; c != 3; ++c) m[r * 4 + c] = rotation[r * 3 + c];
    return MatrixToQuaternion(m, q);
}

inline void QuaternionToWheel(const float* q, float* rotation) {
    float m[16]{};
    QuaternionToMatrix(q, m);
    for (int r = 0; r != 3; ++r)
        for (int c = 0; c != 3; ++c) rotation[r * 3 + c] = m[r * 4 + c];
}

template<class Packet>
class Buffer {
public:
    void Reset() {
        ClearPlayback();
        hasGate_ = false;
        retiredCount_ = 0;
    }

    bool Push(const Packet& packet, std::uint32_t receiveMs) {
        if (!ValidPacket(packet)) return false;
        bool identityChange = false;
        if (hasGate_) {
            if (packet.sessionId == gate_.sessionId) {
                // Gate sequence BEFORE changing worlds: a delayed packet from
                // the previous epoch must never replace the live world.
                if (!Newer(packet.sequence, gate_.sequence)) return false;
                if (packet.worldEpoch != gate_.worldEpoch && !Newer(packet.worldEpoch, gate_.worldEpoch)) return false;
            } else {
                for (std::size_t i = 0; i != retiredCount_; ++i)
                    if (retiredSessions_[i] == packet.sessionId) return false;
                Retire(gate_.sessionId);
            }
            identityChange = packet.sessionId != gate_.sessionId || packet.worldEpoch != gate_.worldEpoch ||
                packet.trackId != gate_.trackId || std::memcmp(packet.carModel, gate_.carModel, sizeof(packet.carModel)) != 0;
        }
        if (packet.flowState != 6) {
            if (count_ || (hasGate_ && gate_.flowState == 6)) ClearPlayback();
            gate_ = packet;
            hasGate_ = true;
            return false;
        }
        if (identityChange && count_) ClearPlayback();
        if (count_) {
            const Node& last = At(count_ - 1);
            const std::int32_t sourceDelta = Delta(packet.sentAtMs, last.packet.sentAtMs);
            const std::int32_t receiveDelta = Delta(receiveMs, lastReceiveMs_);
            if(sourceDelta==0 && receiveDelta>=0 && receiveDelta<=2000 &&
                !Teleport(last.packet,packet,0)) {
                // A valid newer publication can share a coarse source-clock
                // tick with the previous one. Keep its newest pose at the same
                // source instant; resetting playback here made behavior depend
                // on each PC's timer resolution/frame rate.
                receiveTime_+=receiveDelta;lastReceiveMs_=receiveMs;
                history_[(head_+count_-1)%kCapacity].packet=packet;
                gate_=packet;hasGate_=true;return true;
            }
            if (sourceDelta <= 0 || sourceDelta > 2000 || receiveDelta < 0 || receiveDelta > 2000 ||
                std::abs(sourceDelta - receiveDelta) > 2000 || Teleport(last.packet, packet, sourceDelta)) {
                ClearPlayback();
            }
        }
        if (!count_) {
            sourceTime_ = receiveTime_ = 0.0;
            playbackTime_ = -100.0;
            lastRenderTime_ = 0.0;
            Node node{packet, 0.0};
            Append(node);
        } else {
            const double sourceDelta = Delta(packet.sentAtMs, At(count_ - 1).packet.sentAtMs);
            const double receiveDelta = Delta(receiveMs, lastReceiveMs_);
            sourceTime_ += sourceDelta;
            receiveTime_ += receiveDelta;
            const double variation = std::fabs(receiveDelta - sourceDelta);
            jitter_ += 0.1 * (variation - jitter_);
            targetDelay_ = std::clamp(90.0 + 2.5 * jitter_, 90.0, 160.0);
            // A sender's clock may have any uptime offset. Only source deltas
            // and the lowest observed relative transit time establish mapping.
            minimumTransit_ = std::min(minimumTransit_, receiveTime_ - sourceTime_);
            Append(Node{packet, sourceTime_});
            if (hasRendered_ && extrapolation_ > 0) recoveryPending_ = true;
        }
        lastReceiveMs_ = receiveMs;
        gate_ = packet;
        hasGate_ = true;
        return true;
    }

    bool Sample(std::uint32_t now, Packet& output) {
        if (!count_) return false;
        // Relative to the most recent receive anchor so 32-bit local uptime
        // rollover is harmless, without comparing local and sender clocks.
        const double localTime = receiveTime_ + Delta(now, lastReceiveMs_);
        const double elapsed = std::max(0.0, localTime - lastRenderTime_);
        if (elapsed > 0.0) {
            const double target = localTime - minimumTransit_ - targetDelay_;
            const double rate = std::clamp(1.0 + (target - playbackTime_ - elapsed) * 0.002, 0.90, 1.10);
            playbackTime_ += elapsed * rate;
            lastRenderTime_ = localTime;
        }
        effectiveDelay_ = std::clamp(localTime - minimumTransit_ - playbackTime_, 90.0, 160.0);
        const Node& first = At(0);
        const Node& last = At(count_ - 1);
        extrapolation_ = 0;
        if (playbackTime_ <= first.time) {
            output = first.packet;
            SetForward(output);
            FinishSample(output, localTime, elapsed, false);
            return true;
        }
        if (playbackTime_ >= last.time) {
            const double extraMs = std::clamp(playbackTime_ - last.time, 0.0, 100.0);
            extrapolation_ = static_cast<std::uint32_t>(std::lround(extraMs));
            Extrapolate(last.packet, static_cast<float>(extraMs * 0.001), output);
            if (count_ >= 2) {
                const Node& previous = At(count_ - 2);
                const float t = 1.0f + static_cast<float>(extraMs / (last.time - previous.time));
                for (int wheel = 0; wheel != 4; ++wheel) {
                    if ((previous.packet.wheelRotationValidMask & last.packet.wheelRotationValidMask & (1U << wheel)) == 0) continue;
                    float qa[4], qb[4], q[4];
                    WheelQuaternion(previous.packet.wheelRotation[wheel], qa);
                    WheelQuaternion(last.packet.wheelRotation[wheel], qb);
                    Slerp(qa, qb, t, q);
                    QuaternionToWheel(q, output.wheelRotation[wheel]);
                }
            }
            FinishSample(output, localTime, elapsed, extraMs >= 100.0);
            return true;
        }
        for (std::size_t i = 1; i != count_; ++i) {
            const Node& b = At(i);
            if (b.time < playbackTime_) continue;
            const Node& a = At(i - 1);
            const float duration = static_cast<float>((b.time - a.time) * 0.001);
            const float t = static_cast<float>((playbackTime_ - a.time) / (b.time - a.time));
            Interpolate(a.packet, b.packet, t, duration, output);
            FinishSample(output, localTime, elapsed, false);
            return true;
        }
        return false;
    }

    std::size_t Count() const { return count_; }
    std::uint32_t DelayMs() const { return static_cast<std::uint32_t>(std::lround(effectiveDelay_)); }
    std::uint32_t ExtrapolationMs() const { return extrapolation_; }
    std::uint32_t ResetCount() const { return resetCount_; }

private:
    struct Node { Packet packet; double time; };
    static constexpr std::size_t kCapacity = 64;
    Node history_[kCapacity]{};
    std::size_t head_ = 0, count_ = 0;
    Packet gate_{};
    bool hasGate_ = false;
    std::uint64_t retiredSessions_[8]{};
    std::size_t retiredCount_ = 0;
    std::uint32_t lastReceiveMs_ = 0, resetCount_ = 0, extrapolation_ = 0;
    double sourceTime_ = 0.0, receiveTime_ = 0.0, playbackTime_ = -100.0, lastRenderTime_ = 0.0;
    double minimumTransit_ = 0.0, jitter_ = 4.0, targetDelay_ = 100.0, effectiveDelay_ = 100.0;
    Packet rendered_{};
    bool hasRendered_ = false, recoveryPending_ = false, recoveryActive_ = false;
    double recoveryStart_ = 0.0;
    float positionCorrection_[3]{}, rotationCorrection_[4]{0, 0, 0, 1};
    float wheelCorrection_[4][4]{}, phaseCorrection_[4]{};
    std::uint32_t wheelCorrectionMask_ = 0;

    static std::int32_t Delta(std::uint32_t a, std::uint32_t b) {
        const std::uint32_t delta = a - b;
        if (delta <= 0x7fffffffU) return static_cast<std::int32_t>(delta);
        return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(0xffffffffU - delta));
    }
    static bool Newer(std::uint32_t a, std::uint32_t b) { return Delta(a, b) > 0; }
    const Node& At(std::size_t index) const { return history_[(head_ + index) % kCapacity]; }
    void Append(const Node& node) {
        if (count_ == kCapacity) {
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        history_[(head_ + count_) % kCapacity] = node;
        ++count_;
    }
    void ClearPlayback() {
        head_ = count_ = 0;
        sourceTime_ = receiveTime_ = lastRenderTime_ = minimumTransit_ = 0.0;
        playbackTime_ = -100.0;
        jitter_ = 4.0;
        targetDelay_ = effectiveDelay_ = 100.0;
        extrapolation_ = 0;
        hasRendered_ = recoveryPending_ = recoveryActive_ = false;
        wheelCorrectionMask_ = 0;
        ++resetCount_;
    }
    void Retire(std::uint64_t session) {
        if (retiredCount_ == 8) {
            for (std::size_t i = 1; i != 8; ++i) retiredSessions_[i - 1] = retiredSessions_[i];
            --retiredCount_;
        }
        retiredSessions_[retiredCount_++] = session;
    }
    static float Speed(const Packet& p) {
        const float* v = p.linearVelocity;
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
    static bool Teleport(const Packet& a, const Packet& b, std::int32_t durationMs) {
        const double dx = double(b.x) - a.x, dy = double(b.y) - a.y, dz = double(b.z) - a.z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double expected = std::max(Speed(a), Speed(b)) * std::max(0, durationMs) * 0.001;
        return distance > std::max(35.0, expected * 3.0 + 10.0);
    }
    static bool Finite(float value, float limit) { return std::isfinite(value) && std::fabs(value) <= limit; }
    static bool ValidPacket(const Packet& p) {
        if (!Finite(p.x, 1.0e7f) || !Finite(p.y, 1.0e7f) || !Finite(p.z, 1.0e7f) ||
            !Finite(p.speedKmh, 1500.0f) || !Finite(p.headingDeg, 1.0e5f) ||
            !Finite(p.forwardX, 1.1f) || !Finite(p.forwardY, 1.1f) || !Finite(p.forwardZ, 1.1f) ||
            !ValidQuaternion(p.orientation) || (p.wheelRotationValidMask & ~std::uint32_t(15))) return false;
        for (int i = 0; i != 3; ++i)
            if (!Finite(p.linearVelocity[i], 400.0f) || !Finite(p.angularVelocity[i], 200.0f)) return false;
        for (int wheel = 0; wheel != 4; ++wheel) {
            if (!Finite(p.wheelPhaseRad[wheel], 1.0e8f) || !Finite(p.wheelSpeedRad[wheel], 10000.0f)) return false;
            for (int i = 0; i != 9; ++i) if (!Finite(p.wheelRotation[wheel][i], 2.0f)) return false;
            float q[4];
            if ((p.wheelRotationValidMask & (1U << wheel)) && !WheelQuaternion(p.wheelRotation[wheel], q)) return false;
        }
        return true;
    }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static void Multiply(const float* a, const float* b, float* out) {
        const float q[4]{
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
        std::memcpy(out, q, sizeof(q));
        NormalizeQuaternion(out);
    }
    static void Difference(const float* desired, const float* actual, float* difference) {
        const float inverse[4]{-actual[0], -actual[1], -actual[2], actual[3]};
        Multiply(desired, inverse, difference);
        if (difference[3] < 0) for (int i = 0; i != 4; ++i) difference[i] = -difference[i];
    }
    static void StopMotion(Packet& out) {
        for (int i = 0; i != 3; ++i) out.linearVelocity[i] = out.angularVelocity[i] = 0.0f;
        for (int wheel = 0; wheel != 4; ++wheel) out.wheelSpeedRad[wheel] = 0.0f;
        out.speedKmh = 0.0f;
    }
    void FinishSample(Packet& out, double localTime, double elapsed, bool frozen) {
        if (recoveryPending_ && hasRendered_) {
            // Keep the last displayed trajectory continuous for this frame,
            // then shed a small prediction error within 100 ms. Hard resets
            // clear hasRendered_, so teleports/world changes never blend.
            Packet predicted;
            Extrapolate(rendered_, static_cast<float>(std::clamp(elapsed, 0.0, 25.0) * 0.001), predicted);
            positionCorrection_[0] = predicted.x - out.x;
            positionCorrection_[1] = predicted.y - out.y;
            positionCorrection_[2] = predicted.z - out.z;
            const float error2 = positionCorrection_[0] * positionCorrection_[0] +
                positionCorrection_[1] * positionCorrection_[1] + positionCorrection_[2] * positionCorrection_[2];
            // Large disagreements are authoritative corrections, not short
            // prediction errors. Never carry an unbounded position offset.
            recoveryActive_ = error2 <= 100.0f;
            recoveryStart_ = localTime;
            Difference(predicted.orientation, out.orientation, rotationCorrection_);
            wheelCorrectionMask_ = predicted.wheelRotationValidMask & out.wheelRotationValidMask;
            for (int wheel = 0; wheel != 4; ++wheel) {
                phaseCorrection_[wheel] = std::remainder(predicted.wheelPhaseRad[wheel] - out.wheelPhaseRad[wheel], kTau);
                if ((wheelCorrectionMask_ & (1U << wheel)) != 0) {
                    float desired[4], actual[4];
                    WheelQuaternion(predicted.wheelRotation[wheel], desired);
                    WheelQuaternion(out.wheelRotation[wheel], actual);
                    Difference(desired, actual, wheelCorrection_[wheel]);
                }
            }
            recoveryPending_ = false;
        }
        if (recoveryActive_) {
            const float t = static_cast<float>(std::clamp((localTime - recoveryStart_) * 0.01, 0.0, 1.0));
            const float weight = 1.0f - t * t * (3.0f - 2.0f * t);
            const float derivative = 60.0f * (t * t - t); // per second
            out.x += positionCorrection_[0] * weight;
            out.y += positionCorrection_[1] * weight;
            out.z += positionCorrection_[2] * weight;
            for (int i = 0; i != 3; ++i)
                out.linearVelocity[i] = std::clamp(out.linearVelocity[i] + positionCorrection_[i] * derivative, -400.0f, 400.0f);
            const float identity[4]{0, 0, 0, 1};
            float correction[4];
            Slerp(identity, rotationCorrection_, weight, correction);
            Multiply(correction, out.orientation, out.orientation);
            for (int wheel = 0; wheel != 4; ++wheel) {
                out.wheelPhaseRad[wheel] = WrapPhase(out.wheelPhaseRad[wheel] + phaseCorrection_[wheel] * weight);
                if ((wheelCorrectionMask_ & out.wheelRotationValidMask & (1U << wheel)) != 0) {
                    float actual[4], corrected[4];
                    WheelQuaternion(out.wheelRotation[wheel], actual);
                    Slerp(identity, wheelCorrection_[wheel], weight, correction);
                    Multiply(correction, actual, corrected);
                    QuaternionToWheel(corrected, out.wheelRotation[wheel]);
                }
            }
            if (t >= 1.0f) recoveryActive_ = false;
            SetForward(out);
        }
        // A native rigid body must receive zero velocity once prediction is
        // capped, otherwise the game would continue integrating after hold.
        if (frozen) StopMotion(out);
        if (!ValidPacket(out)) {
            out = At(count_ - 1).packet;
            StopMotion(out);
            SetForward(out);
            recoveryActive_ = false;
        }
        rendered_ = out;
        hasRendered_ = true;
    }
    static void SetForward(Packet& p) {
        float m[16];
        QuaternionToMatrix(p.orientation, m);
        p.forwardX = m[0]; p.forwardY = m[1]; p.forwardZ = m[2];
        p.headingDeg = std::atan2(m[1], m[0]) * (180.0f / kPi);
        if (p.headingDeg < 0.0f) p.headingDeg += 360.0f;
    }
    static void Interpolate(const Packet& a, const Packet& b, float t, float duration, Packet& out) {
        // Preserve discrete race/effect/custom fields from one actual snapshot.
        out = t < 0.5f ? a : b;
        const float pa[3]{a.x, a.y, a.z}, pb[3]{b.x, b.y, b.z};
        float position[3], linear[3];
        const float t2 = t * t, t3 = t2 * t;
        float distance2 = 0.0f, deviation2 = 0.0f;
        bool hermite = duration <= 0.5f;
        for (int i = 0; i != 3; ++i) {
            const float displacement = pb[i] - pa[i];
            linear[i] = Lerp(pa[i], pb[i], t);
            position[i] = (2 * t3 - 3 * t2 + 1) * pa[i] + (t3 - 2 * t2 + t) * duration * a.linearVelocity[i] +
                (-2 * t3 + 3 * t2) * pb[i] + (t3 - t2) * duration * b.linearVelocity[i];
            distance2 += displacement * displacement;
            const float deviation = position[i] - linear[i];
            deviation2 += deviation * deviation;
            const float pad = 0.10f + std::fabs(displacement) * 0.10f;
            if (position[i] < std::min(pa[i], pb[i]) - pad || position[i] > std::max(pa[i], pb[i]) + pad) hermite = false;
            out.linearVelocity[i] = Lerp(a.linearVelocity[i], b.linearVelocity[i], t);
            out.angularVelocity[i] = Lerp(a.angularVelocity[i], b.angularVelocity[i], t);
        }
        const float distance = std::sqrt(distance2);
        const float maxTangent = std::max(Speed(a), Speed(b)) * duration;
        if (maxTangent > 3 * distance + 2.0f || std::sqrt(deviation2) > std::max(0.15f, distance * 0.25f)) hermite = false;
        out.x = hermite ? position[0] : linear[0];
        out.y = hermite ? position[1] : linear[1];
        out.z = hermite ? position[2] : linear[2];
        out.speedKmh = Lerp(a.speedKmh, b.speedKmh, t);
        out.steerAngle = static_cast<decltype(out.steerAngle)>(std::lround(Lerp(float(a.steerAngle), float(b.steerAngle), t)));
        Slerp(a.orientation, b.orientation, t, out.orientation);
        SetForward(out);
        for (int wheel = 0; wheel != 4; ++wheel) {
            out.wheelSpeedRad[wheel] = Lerp(a.wheelSpeedRad[wheel], b.wheelSpeedRad[wheel], t);
            const float expected = (a.wheelSpeedRad[wheel] + b.wheelSpeedRad[wheel]) * 0.5f * duration;
            float phaseDelta = std::remainder(b.wheelPhaseRad[wheel] - a.wheelPhaseRad[wheel], kTau);
            phaseDelta += kTau * std::round((expected - phaseDelta) / kTau);
            out.wheelPhaseRad[wheel] = WrapPhase(a.wheelPhaseRad[wheel] + phaseDelta * t);
            if ((a.wheelRotationValidMask & b.wheelRotationValidMask & (1U << wheel)) != 0) {
                float qa[4], qb[4], q[4];
                WheelQuaternion(a.wheelRotation[wheel], qa);
                WheelQuaternion(b.wheelRotation[wheel], qb);
                Slerp(qa, qb, t, q);
                QuaternionToWheel(q, out.wheelRotation[wheel]);
            }
        }
    }
    static void Extrapolate(const Packet& p, float seconds, Packet& out) {
        out = p;
        out.x += p.linearVelocity[0] * seconds;
        out.y += p.linearVelocity[1] * seconds;
        out.z += p.linearVelocity[2] * seconds;
        const float* v = p.angularVelocity;
        const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed > 0.00001f && seconds > 0.0f) {
            const float scale = std::sin(speed * seconds * 0.5f) / speed;
            const float d[4]{v[0] * scale, v[1] * scale, v[2] * scale, std::cos(speed * seconds * 0.5f)};
            const float* q = p.orientation;
            out.orientation[0] = d[3] * q[0] + d[0] * q[3] + d[1] * q[2] - d[2] * q[1];
            out.orientation[1] = d[3] * q[1] - d[0] * q[2] + d[1] * q[3] + d[2] * q[0];
            out.orientation[2] = d[3] * q[2] + d[0] * q[1] - d[1] * q[0] + d[2] * q[3];
            out.orientation[3] = d[3] * q[3] - d[0] * q[0] - d[1] * q[1] - d[2] * q[2];
            NormalizeQuaternion(out.orientation);
        }
        SetForward(out);
        for (int wheel = 0; wheel != 4; ++wheel)
            out.wheelPhaseRad[wheel] = WrapPhase(p.wheelPhaseRad[wheel] + p.wheelSpeedRad[wheel] * seconds);
        // Tire/rim angular motion is extrapolated separately from two measured
        // matrices; wheelSpeedRad belongs to optional spinners, not tire spin.
    }
};

} // namespace coop_motion
