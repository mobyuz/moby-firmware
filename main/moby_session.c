// SPDX-License-Identifier: GPL-3.0-only

#include "moby_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "moby_font.h"

_Static_assert(MOBY_FACE_IDLE + 0 == MOBY_EXPRESSION_IDLE + 0 &&
                   MOBY_FACE_LISTENING + 0 == MOBY_EXPRESSION_LISTENING + 0 &&
                   MOBY_FACE_THINKING + 0 == MOBY_EXPRESSION_THINKING + 0 &&
                   MOBY_FACE_SPEAKING + 0 == MOBY_EXPRESSION_SPEAKING + 0 &&
                   MOBY_FACE_HAPPY + 0 == MOBY_EXPRESSION_HAPPY + 0 &&
                   MOBY_FACE_CONFUSED + 0 == MOBY_EXPRESSION_CONFUSED + 0 &&
                   MOBY_FACE_ERROR + 0 == MOBY_EXPRESSION_ERROR + 0 &&
                   MOBY_FACE_MUTED + 0 == MOBY_EXPRESSION_MUTED + 0 &&
                   MOBY_FACE_DISCONNECTED + 0 == MOBY_EXPRESSION_DISCONNECTED + 0 &&
                   MOBY_FACE_COUNT + 0 == MOBY_EXPRESSION_COUNT + 0,
               "face expressions must follow the protocol order");

_Static_assert(MOBY_FACE_TEXT_MAX == MOBY_USB_SCREEN_TEXT_MAX_CHARS, "the face and the protocol bound display text alike");

static bool due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void note(const moby_session_t *session, const char *line)
{
    if (session->ops.log) {
        session->ops.log(session->ops.ctx, line);
    }
}

static const char *state_name(moby_session_state_t state)
{
    static const char *const names[] = { "boot", "disconnected", "pairing", "idle", "listening", "thinking", "speaking" };
    return names[state];
}

static void set_state(moby_session_t *session, moby_session_state_t state)
{
    if (session->state != state) {
        char line[48];
        snprintf(line, sizeof line, "session state=%s", state_name(state));
        session->state = state;
        note(session, line);
    }
}

// The expression shown now. The hardware mute wins, then a missing session, then an error.
static moby_expression effective(const moby_session_t *session)
{
    if (session->muted) {
        return MOBY_EXPRESSION_MUTED;
    }
    if (!session->established) {
        return MOBY_EXPRESSION_DISCONNECTED;
    }
    // Listening is not a face the desktop can pick: it is shown exactly while capture is on.
    if (session->stream.phase == MOBY_STREAM_CAPTURING) {
        return MOBY_EXPRESSION_LISTENING;
    }
    return session->error_active ? MOBY_EXPRESSION_ERROR : session->face;
}

static moby_out_t *push(moby_session_t *session, uint32_t now_ms, moby_out_kind_t kind)
{
    if (!session->link_up) {
        return NULL;
    }
    if (session->outbox_count == MOBY_OUTBOX_LEN) {
        ++session->stats.outbox_dropped;
        return NULL;
    }
    moby_out_t *out = &session->outbox[(session->outbox_head + session->outbox_count++) % MOBY_OUTBOX_LEN];
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->ts_ms = now_ms;
    return out;
}

static moby_out_t *push_event(moby_session_t *session, uint32_t now_ms, moby_out_kind_t kind)
{
    moby_out_t *out = push(session, now_ms, kind);
    if (out) {
        snprintf(out->id, sizeof out->id, "e-%04" PRIu32, session->event_counter++ % 10000U);
    }
    return out;
}

static void emit_error(moby_session_t *session, uint32_t now_ms, const char *code, const char *detail)
{
    moby_out_t *out = push_event(session, now_ms, MOBY_OUT_ERROR);
    if (out) {
        out->u.error.code = code;
        out->u.error.detail = detail;
    }
}

// An error the robot raises itself. It also shows the error face for a moment.
static void raise_error(moby_session_t *session, uint32_t now_ms, const char *code, const char *detail)
{
    emit_error(session, now_ms, code, detail);
    if (!session->muted && session->established) {
        session->error_active = true;
        session->error_until_ms = now_ms + MOBY_ERROR_FACE_MS;
    }
}

static void respond(moby_session_t *session, uint32_t now_ms, const char *id, const char *code, const char *detail)
{
    moby_out_t *out = push(session, now_ms, code ? MOBY_OUT_COMMAND_ERROR : MOBY_OUT_COMMAND_OK);
    if (out) {
        strcpy(out->id, id);
        out->u.error.code = code;
        out->u.error.detail = detail;
    }
}

// --- Push to talk -----------------------------------------------------------------------------

typedef enum {
    // Capture is off now; the buffered audio is sent, then audio_end.
    STOP_DRAIN,
    // Capture is off and the buffer is thrown away; audio_end follows at once.
    STOP_DISCARD,
    // Capture is off and the buffer is thrown away; nobody is told (the link is gone or the start never arrived).
    STOP_SILENT,
} stop_mode_t;

static bool capture_available(const moby_session_t *session)
{
    return (session->identity.capabilities & MOBY_CAP_USB_PCM) != 0 && session->ops.capture_set != NULL &&
           session->ops.capture_discard != NULL;
}

// The only place that changes the phase. It calls capture_set exactly when the capturing state changes, so the
// microphone, the LED and the sample gate move together with the face.
static void enter_phase(moby_session_t *session, moby_stream_phase_t phase)
{
    const bool was = session->stream.phase == MOBY_STREAM_CAPTURING;
    const bool now = phase == MOBY_STREAM_CAPTURING;
    session->stream.phase = phase;
    if (was != now && session->ops.capture_set) {
        session->ops.capture_set(session->ops.ctx, now);
    }
}

static void flush_end(moby_session_t *session, uint32_t now_ms)
{
    if (!session->stream.end_owed) {
        return;
    }
    moby_out_t *out = push_event(session, now_ms, MOBY_OUT_AUDIO_END);
    if (out) {
        out->u.audio_end.stream_id = session->stream.end_id;
        out->u.audio_end.reason = session->stream.end_reason;
        out->u.audio_end.frames = session->stream.end_frames;
        out->u.audio_end.duration_ms = session->stream.end_frames * (uint32_t)MOBY_PCM_FRAME_MS;
        session->stream.end_owed = false;
    }
}

static void finish_stream(moby_session_t *session, uint32_t now_ms, moby_audio_end_reason_t reason, bool announce)
{
    enter_phase(session, MOBY_STREAM_NONE);
    if (session->ops.capture_discard) {
        session->ops.capture_discard(session->ops.ctx);
    }
    if (session->state == MOBY_SESSION_LISTENING) {
        set_state(session, MOBY_SESSION_IDLE);
    }
    if (announce && session->link_up) {
        session->stream.end_owed = true;
        session->stream.end_reason = reason;
        session->stream.end_id = session->stream.id;
        session->stream.end_frames = session->stream.frames;
        flush_end(session, now_ms);
    }
}

static void stream_stop(moby_session_t *session, uint32_t now_ms, moby_audio_end_reason_t reason, stop_mode_t mode)
{
    if (session->stream.phase == MOBY_STREAM_NONE) {
        return;
    }
    if (mode == STOP_DRAIN) {
        if (session->stream.phase != MOBY_STREAM_CAPTURING) {
            return;
        }
        // Capture stops now: the gate closes, the LED goes dark and the face leaves listening. Only the audio that was
        // captured while the button was held is still sent.
        enter_phase(session, MOBY_STREAM_DRAINING);
        session->stream.drain_since_ms = now_ms;
        session->stream.end_reason = reason;
        if (session->state == MOBY_SESSION_LISTENING) {
            set_state(session, MOBY_SESSION_IDLE);
        }
        session->face = MOBY_EXPRESSION_IDLE;
        note(session, "capture off");
        return;
    }
    if (session->stream.phase == MOBY_STREAM_CAPTURING) {
        session->face = MOBY_EXPRESSION_IDLE;
    }
    finish_stream(session, now_ms, reason, mode == STOP_DISCARD);
    note(session, "capture off");
}

// A failure that ends the stream: the desktop gets audio_end with reason error, an error event, and the face blinks
// the error face for a moment.
static void stream_fail(moby_session_t *session, uint32_t now_ms, const char *code, const char *detail)
{
    if (session->stream.phase == MOBY_STREAM_NONE) {
        return;
    }
    stream_stop(session, now_ms, MOBY_AUDIO_END_ERROR, STOP_DISCARD);
    raise_error(session, now_ms, code, detail);
}

// Every condition for opening the microphone, in one place. Anything else leaves it off.
static bool can_start(const moby_session_t *session)
{
    return capture_available(session) && session->link_up && session->established && session->authenticated &&
           !session->muted && session->talk_down && session->state == MOBY_SESSION_IDLE &&
           session->stream.phase == MOBY_STREAM_NONE && !session->stream.end_owed &&
           moby_can_transition(effective(session), MOBY_EXPRESSION_LISTENING);
}

static void stream_start(moby_session_t *session, uint32_t now_ms)
{
    if (!can_start(session)) {
        return;
    }
    // The desktop must be told before any audio exists. If the outbox cannot take audio_start, nothing starts.
    moby_out_t *out = push_event(session, now_ms, MOBY_OUT_AUDIO_START);
    if (!out) {
        return;
    }
    uint16_t id = (uint16_t)(session->stream.last_id + 1U);
    if (id < MOBY_USB_AUDIO_STREAM_ID_MIN) {
        id = MOBY_USB_AUDIO_STREAM_ID_MIN;
    }
    session->stream.last_id = id;
    session->stream.id = id;
    session->stream.frames = 0;
    session->stream.started_ms = now_ms;
    out->u.audio_start.stream_id = id;
    out->u.audio_start.reason = MOBY_AUDIO_START_BUTTON;
    out->u.audio_start.preroll_ms = 0;
    session->error_active = false;
    session->expression_timed = false;
    session->face = MOBY_EXPRESSION_IDLE;
    set_state(session, MOBY_SESSION_LISTENING);
    ++session->stats.streams_started;
    enter_phase(session, MOBY_STREAM_CAPTURING);
    note(session, "capture on");
}

static void clear_pairing(moby_session_t *session)
{
    session->pair_confirmed = false;
    if (session->state == MOBY_SESSION_PAIRING) {
        set_state(session, MOBY_SESSION_IDLE);
    }
}

static void send_hello(moby_session_t *session, uint32_t now_ms)
{
    moby_out_t *out = push(session, now_ms, MOBY_OUT_HELLO);
    if (out) {
        strcpy(out->id, session->hello_id);
        memcpy(out->u.hello.nonce, session->nonce, sizeof session->nonce);
        out->u.hello.mute = session->muted;
        out->u.hello.paired = session->paired;
    }
    session->hello_sent_ms = now_ms;
}

// Starts a handshake attempt with a fresh nonce and hello id.
static void start_attempt(moby_session_t *session, uint32_t now_ms)
{
    session->hello_sent_ms = now_ms;
    session->hello_open = false;
    session->hello_rejected = false;
    if (!session->ops.fill_random || !session->ops.fill_random(session->ops.ctx, session->nonce, sizeof session->nonce)) {
        memset(session->nonce, 0, sizeof session->nonce);
        note(session, "hello unavailable: no random bytes");
        return;
    }
    snprintf(session->hello_id, sizeof session->hello_id, "e-%04" PRIu32, session->event_counter++ % 10000U);
    session->hello_open = true;
    session->attempt_ms = now_ms;
    send_hello(session, now_ms);
}

static void end_session(moby_session_t *session, uint32_t now_ms, const char *reason)
{
    // Nobody is told: the link is gone or the desktop stopped answering. Capture stops first, the buffer is wiped.
    stream_stop(session, now_ms, MOBY_AUDIO_END_ERROR, STOP_SILENT);
    session->stream.end_owed = false;
    if (session->ops.capture_discard) {
        session->ops.capture_discard(session->ops.ctx);
    }
    session->established = false;
    session->authenticated = false;
    session->session_id[0] = '\0';
    session->pair_confirmed = false;
    session->error_active = false;
    session->expression_timed = false;
    session->text[0] = '\0';
    session->face = MOBY_EXPRESSION_IDLE;
    session->unanswered = 0;
    session->hello_open = false;
    session->hello_rejected = false;
    session->rx_sequence_known = false;
    session->press_announced = false;
    session->outbox_count = 0;
    memset(session->nonce, 0, sizeof session->nonce);
    set_state(session, MOBY_SESSION_DISCONNECTED);
    note(session, reason);
    if (session->link_up) {
        start_attempt(session, now_ms);
    }
}

void moby_session_init(moby_session_t *session, const moby_identity_t *identity, const uint8_t *pair_key,
                       const moby_session_ops_t *ops, uint32_t now_ms)
{
    memset(session, 0, sizeof *session);
    session->ops = *ops;
    session->identity = *identity;
    session->state = MOBY_SESSION_BOOT;
    session->face = MOBY_EXPRESSION_IDLE;
    session->muted = true;
    session->attempt_ms = now_ms;
    session->config.wake_sensitivity = 50;
    session->config.speaker_volume = 50;
    session->config.vad_silence_ms = 800;
    session->config.max_utterance_ms = 15000;
    if (pair_key) {
        session->paired = true;
        memcpy(session->pair_key, pair_key, sizeof session->pair_key);
    }
}

void moby_session_set_link(moby_session_t *session, uint32_t now_ms, bool up)
{
    if (up == session->link_up) {
        return;
    }
    if (up) {
        session->link_up = true;
        session->rx_sequence_known = false;
        if (session->state != MOBY_SESSION_BOOT) {
            start_attempt(session, now_ms);
        }
        return;
    }
    // The link is gone, so there is nobody to tell. End the session without queueing anything.
    session->link_up = false;
    end_session(session, now_ms, "link down");
}

static void apply_mute(moby_session_t *session, uint32_t now_ms, bool muted)
{
    const bool was_pairing = session->state == MOBY_SESSION_PAIRING;
    if (muted) {
        // First of all: the microphone goes off and the buffered audio is thrown away, before any message is queued.
        session->muted = true;
        stream_stop(session, now_ms, MOBY_AUDIO_END_MUTE, STOP_DISCARD);
    }
    session->muted = muted;
    session->pair_confirmed = false;
    session->expression_timed = false;
    session->error_active = false;
    session->face = MOBY_EXPRESSION_IDLE;
    if (muted) {
        session->text[0] = '\0';
    }
    // A mute interrupts any state and an unmute first goes to idle.
    if (session->established) {
        set_state(session, MOBY_SESSION_IDLE);
        moby_out_t *out = push_event(session, now_ms, MOBY_OUT_MUTE_CHANGED);
        if (out) {
            out->u.mute.muted = muted;
        }
        if (muted && was_pairing) {
            emit_error(session, now_ms, MOBY_USB_E_MUTED, "pairing cancelled by mute");
        }
    }
    note(session, muted ? "mute=on" : "mute=off");
}

static void wipe_pairing(moby_session_t *session, uint32_t now_ms)
{
    if (session->ops.erase_key && !session->ops.erase_key(session->ops.ctx)) {
        emit_error(session, now_ms, MOBY_USB_E_HW, "pairing wipe failed");
        return;
    }
    memset(session->pair_key, 0, sizeof session->pair_key);
    session->paired = false;
    session->authenticated = false;
    clear_pairing(session);
    note(session, "pairing wiped");
    if (session->established) {
        push_event(session, now_ms, MOBY_OUT_PAIR_RESET);
    }
}

static void on_press(moby_session_t *session, uint32_t now_ms)
{
    if (!session->established) {
        return;
    }
    if (session->state == MOBY_SESSION_PAIRING && !session->pair_confirmed) {
        // The press confirms pairing instead of starting anything else.
        session->pair_confirmed = true;
        push_event(session, now_ms, MOBY_OUT_PAIR_BUTTON_CONFIRMED);
        note(session, "pairing button confirmed");
        return;
    }
    moby_out_t *out = push_event(session, now_ms, MOBY_OUT_BUTTON_EVENT);
    if (out) {
        out->u.button.action = MOBY_BUTTON_PRESS;
        session->press_announced = true;
    }
    stream_start(session, now_ms);
}

static void on_release(moby_session_t *session, uint32_t now_ms)
{
    stream_stop(session, now_ms, MOBY_AUDIO_END_BUTTON_RELEASE, STOP_DRAIN);
    if (session->press_announced) {
        session->press_announced = false;
        moby_out_t *out = push_event(session, now_ms, MOBY_OUT_BUTTON_EVENT);
        if (out) {
            out->u.button.action = MOBY_BUTTON_RELEASE;
        }
    }
}

void moby_session_controls(moby_session_t *session, uint32_t now_ms, bool muted, bool talk_down, bool attention)
{
    if (muted != session->muted) {
        apply_mute(session, now_ms, muted);
    }
    attention = attention && !muted;
    if (talk_down != session->talk_down) {
        session->talk_down = talk_down;
        session->talk_since_ms = now_ms;
        session->long_fired = false;
    }
    if (attention && !session->attention) {
        on_press(session, now_ms);
    } else if (!attention && session->attention) {
        on_release(session, now_ms);
    }
    session->attention = attention;
    if (talk_down && !session->long_fired && due(now_ms, session->talk_since_ms + MOBY_LONG_PRESS_MS)) {
        session->long_fired = true;
        // The pairing is about to be erased, so nothing may keep recording under it.
        stream_stop(session, now_ms, MOBY_AUDIO_END_CANCEL, STOP_DISCARD);
        if (session->established) {
            moby_out_t *out = push_event(session, now_ms, MOBY_OUT_BUTTON_EVENT);
            if (out) {
                out->u.button.action = MOBY_BUTTON_LONG_PRESS;
            }
        }
        wipe_pairing(session, now_ms);
    }
}

static void tick_timers(moby_session_t *session, uint32_t now_ms)
{
    if (session->error_active && due(now_ms, session->error_until_ms)) {
        session->error_active = false;
    }
    if (session->text[0] && due(now_ms, session->text_until_ms)) {
        session->text[0] = '\0';
    }
    if (session->expression_timed && due(now_ms, session->expression_until_ms)) {
        session->expression_timed = false;
        session->face = MOBY_EXPRESSION_IDLE;
        if (session->state != MOBY_SESSION_PAIRING) {
            set_state(session, MOBY_SESSION_IDLE);
        }
    }
    if (session->state == MOBY_SESSION_PAIRING && due(now_ms, session->pair_deadline_ms)) {
        clear_pairing(session);
        raise_error(session, now_ms, MOBY_USB_E_TIMEOUT, "pairing window expired");
    }
    if (session->stream.phase == MOBY_STREAM_CAPTURING &&
        due(now_ms, session->stream.started_ms + session->config.max_utterance_ms)) {
        stream_stop(session, now_ms, MOBY_AUDIO_END_MAX_DURATION, STOP_DRAIN);
    } else if (session->stream.phase == MOBY_STREAM_DRAINING &&
               due(now_ms, session->stream.drain_since_ms + MOBY_DRAIN_MS)) {
        stream_fail(session, now_ms, MOBY_USB_E_AUDIO_OVERRUN, "audio was not collected in time");
    }
    if (session->link_up) {
        flush_end(session, now_ms);
    }
}

void moby_session_tick(moby_session_t *session, uint32_t now_ms)
{
    if (session->state == MOBY_SESSION_BOOT) {
        set_state(session, MOBY_SESSION_DISCONNECTED);
        if (session->link_up) {
            start_attempt(session, now_ms);
        }
    }
    tick_timers(session, now_ms);
    if (!session->link_up) {
        return;
    }
    if (!session->established) {
        if (!session->hello_open) {
            if (due(now_ms, session->hello_sent_ms + MOBY_HELLO_RETRY_MS)) {
                start_attempt(session, now_ms);
            }
        } else if (due(now_ms, session->attempt_ms + MOBY_HANDSHAKE_MS)) {
            start_attempt(session, now_ms);
        } else if (!session->hello_rejected && due(now_ms, session->hello_sent_ms + MOBY_HELLO_RETRY_MS)) {
            send_hello(session, now_ms);
        }
        return;
    }
    if (due(now_ms, session->next_heartbeat_ms)) {
        if (session->unanswered >= MOBY_LIVENESS_MISSED) {
            end_session(session, now_ms, "desktop silent");
            return;
        }
        moby_out_t *out = push_event(session, now_ms, MOBY_OUT_HEARTBEAT);
        if (out) {
            out->u.heartbeat.state = effective(session);
            out->u.heartbeat.mute = session->muted;
            out->u.heartbeat.uptime_ms = now_ms;
        }
        ++session->unanswered;
        session->next_heartbeat_ms = now_ms + MOBY_HEARTBEAT_MS;
    }
}

void moby_session_note_frame(moby_session_t *session, uint32_t sequence)
{
    if (session->rx_sequence_known && sequence != session->rx_sequence_next) {
        ++session->stats.rx_sequence_gaps;
    }
    session->rx_sequence_known = true;
    session->rx_sequence_next = sequence + 1;
    if (session->established) {
        session->unanswered = 0;
    }
}

void moby_session_protocol_error(moby_session_t *session, uint32_t now_ms, const char *code, const char *detail)
{
    if (session->error_sent && (uint32_t)(now_ms - session->error_sent_ms) < MOBY_ERROR_GAP_MS) {
        ++session->stats.errors_suppressed;
        return;
    }
    session->error_sent = true;
    session->error_sent_ms = now_ms;
    emit_error(session, now_ms, code, detail);
}

void moby_session_reject(moby_session_t *session, uint32_t now_ms, const char *command_id, const char *code,
                         const char *detail)
{
    if (command_id) {
        respond(session, now_ms, command_id, code, detail);
    } else {
        moby_session_protocol_error(session, now_ms, code, detail);
    }
}

static bool proof_ok(const moby_session_t *session, const moby_msg_t *msg)
{
    uint8_t input[MOBY_USB_NONCE_BYTES + 2 * MOBY_ID_MAX + 2];
    uint8_t expected[MOBY_USB_PROOF_BYTES];
    size_t length = 0;
    if (!msg->u.hello_ack.has_proof || !session->ops.hmac_sha256 ||
        moby_session_proof_input(session->nonce, session->identity.device_id, msg->u.hello_ack.session_id, input,
                                 sizeof input, &length) != MOBY_OK ||
        !session->ops.hmac_sha256(session->ops.ctx, session->pair_key, sizeof session->pair_key, input, length, expected)) {
        return false;
    }
    const bool match = moby_ct_equal(expected, msg->u.hello_ack.proof, sizeof expected);
    memset(expected, 0, sizeof expected);
    return match;
}

static void apply_patch(moby_session_t *session, const moby_config_patch_t *patch)
{
    if (patch->has_wake_sensitivity) {
        session->config.wake_sensitivity = patch->wake_sensitivity;
    }
    if (patch->has_speaker_volume) {
        session->config.speaker_volume = patch->speaker_volume;
    }
    if (patch->has_vad_silence_ms) {
        session->config.vad_silence_ms = patch->vad_silence_ms;
    }
    if (patch->has_max_utterance_ms) {
        session->config.max_utterance_ms = patch->max_utterance_ms;
    }
}

static void on_hello_ack(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    // A late or repeated ack never restarts or re-authenticates a session.
    if (session->established || !session->hello_open || strcmp(msg->id, session->hello_id) != 0) {
        return;
    }
    if (!msg->u.hello_ack.accepted) {
        session->hello_rejected = true;
        note(session, "hello rejected by desktop");
        return;
    }
    if (!msg->u.hello_ack.has_session_id) {
        moby_session_protocol_error(session, now_ms, MOBY_USB_E_BAD_FRAME, "hello_ack without session_id");
        return;
    }
    const bool proven = !session->paired || proof_ok(session, msg);
    session->authenticated = session->paired && proven;
    session->established = true;
    session->hello_open = false;
    strcpy(session->session_id, msg->u.hello_ack.session_id);
    memset(session->nonce, 0, sizeof session->nonce);
    session->unanswered = 0;
    session->next_heartbeat_ms = now_ms + MOBY_HEARTBEAT_MS;
    session->face = MOBY_EXPRESSION_IDLE;
    // Settings from an ack the robot could not verify are ignored, like every other command of such a session.
    if (proven) {
        apply_patch(session, &msg->u.hello_ack.config);
    }
    set_state(session, MOBY_SESSION_IDLE);
    note(session, session->authenticated ? "session established authenticated=1" : "session established authenticated=0");
    moby_out_t *out = push_event(session, now_ms, MOBY_OUT_MUTE_CHANGED);
    if (out) {
        out->u.mute.muted = session->muted;
        out->u.mute.boot = true;
    }
    if (!proven) {
        raise_error(session, now_ms, MOBY_USB_E_STATE, "authentication failed");
    }
}

static bool animation_known(const char *name)
{
    return strcmp(name, "none") == 0 || strcmp(name, "blink") == 0;
}

static moby_session_state_t state_for(moby_expression expression)
{
    switch (expression) {
    case MOBY_EXPRESSION_LISTENING:
        return MOBY_SESSION_LISTENING;
    case MOBY_EXPRESSION_THINKING:
        return MOBY_SESSION_THINKING;
    case MOBY_EXPRESSION_SPEAKING:
        return MOBY_SESSION_SPEAKING;
    default:
        return MOBY_SESSION_IDLE;
    }
}

static void on_expression(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    const char *id = msg->id;
    const moby_expression target = msg->u.expression.state;
    if (session->muted) {
        respond(session, now_ms, id, MOBY_USB_E_MUTED, "hardware mute is on");
    } else if (session->state == MOBY_SESSION_PAIRING) {
        respond(session, now_ms, id, MOBY_USB_E_BUSY, "pairing in progress");
    } else if (session->stream.phase == MOBY_STREAM_CAPTURING) {
        respond(session, now_ms, id, MOBY_USB_E_BUSY, "capture in progress");
    } else if (target == MOBY_EXPRESSION_MUTED || target == MOBY_EXPRESSION_DISCONNECTED ||
               target == MOBY_EXPRESSION_LISTENING) {
        // Listening, like mute and disconnected, is a fact about the robot: it is shown while capture is on.
        respond(session, now_ms, id, MOBY_USB_E_STATE, "the robot owns this state");
    } else if (!animation_known(msg->u.expression.animation)) {
        respond(session, now_ms, id, MOBY_USB_E_UNSUPPORTED, "unknown animation");
    } else if (!moby_can_transition(effective(session), target)) {
        respond(session, now_ms, id, MOBY_USB_E_STATE, "transition not allowed");
    } else {
        session->error_active = false;
        session->face = target;
        session->expression_timed = msg->u.expression.duration_ms != 0 && target != MOBY_EXPRESSION_IDLE;
        session->expression_until_ms = now_ms + msg->u.expression.duration_ms;
        set_state(session, state_for(target));
        respond(session, now_ms, id, NULL, NULL);
    }
}

// Turns the UTF-8 text into display cells: ASCII, Russian and Uzbek Cyrillic and the Uzbek turned comma
// show as themselves, anything else as '?', one cell per code point.
static void set_text(moby_session_t *session, const char *utf8)
{
    moby_font_decode(utf8, session->text, MOBY_USB_SCREEN_TEXT_MAX_CHARS);
}

static void on_pair_begin(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    const char *id = msg->id;
    if (session->muted) {
        respond(session, now_ms, id, MOBY_USB_E_MUTED, "hardware mute is on");
    } else if (session->paired) {
        // A paired robot never takes a new key by command, so a local process cannot replace it.
        respond(session, now_ms, id, MOBY_USB_E_STATE, "already paired");
    } else if (session->state == MOBY_SESSION_PAIRING) {
        respond(session, now_ms, id, MOBY_USB_E_BUSY, "pairing already waiting");
    } else if (session->state != MOBY_SESSION_IDLE) {
        respond(session, now_ms, id, MOBY_USB_E_STATE, "robot is not idle");
    } else {
        session->pair_confirmed = false;
        session->pair_deadline_ms = now_ms + msg->u.pair_begin.timeout_ms;
        session->face = MOBY_EXPRESSION_IDLE;
        session->expression_timed = false;
        set_state(session, MOBY_SESSION_PAIRING);
        note(session, "pairing begun");
        respond(session, now_ms, id, NULL, NULL);
    }
}

static void on_pair_set_key(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    const char *id = msg->id;
    if (session->paired || session->state != MOBY_SESSION_PAIRING || !session->pair_confirmed) {
        respond(session, now_ms, id, MOBY_USB_E_STATE, "pairing was not confirmed");
    } else if (session->ops.store_key && !session->ops.store_key(session->ops.ctx, msg->u.pair_set_key.key)) {
        respond(session, now_ms, id, MOBY_USB_E_HW, "could not store the key");
    } else {
        memcpy(session->pair_key, msg->u.pair_set_key.key, sizeof session->pair_key);
        session->paired = true;
        session->authenticated = true;
        clear_pairing(session);
        note(session, "paired");
        respond(session, now_ms, id, NULL, NULL);
    }
}

static void on_command(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    const char *id = msg->id;
    if (!session->established) {
        respond(session, now_ms, id, MOBY_USB_E_STATE, "no session");
        return;
    }
    // A missing capability is a fixed property of the robot, so it is checked first.
    if (msg->name == MOBY_MSG_AUDIO_PLAY_START || msg->name == MOBY_MSG_AUDIO_PLAY_END) {
        respond(session, now_ms, id, session->identity.capabilities & MOBY_CAP_USB_PCM ? MOBY_USB_E_STATE : MOBY_USB_E_UNSUPPORTED,
                session->identity.capabilities & MOBY_CAP_USB_PCM ? "audio playback not implemented" : "audio unsupported");
        return;
    }
    if (msg->name == MOBY_MSG_SERVO_POSE) {
        respond(session, now_ms, id, session->identity.capabilities & MOBY_CAP_SERVO_POSE ? MOBY_USB_E_STATE : MOBY_USB_E_UNSUPPORTED,
                session->identity.capabilities & MOBY_CAP_SERVO_POSE ? "servo not implemented" : "servo unsupported");
        return;
    }
    // A paired robot obeys only a desktop that proved the key.
    if (session->paired && !session->authenticated) {
        respond(session, now_ms, id, MOBY_USB_E_STATE, "not authenticated");
        return;
    }
    switch (msg->name) {
    case MOBY_MSG_EXPRESSION:
        on_expression(session, now_ms, msg);
        break;
    case MOBY_MSG_DISPLAY_TEXT:
        if (session->muted) {
            respond(session, now_ms, id, MOBY_USB_E_MUTED, "hardware mute is on");
        } else {
            set_text(session, msg->u.display_text.text);
            session->text_until_ms = now_ms + msg->u.display_text.ttl_ms;
            respond(session, now_ms, id, NULL, NULL);
        }
        break;
    case MOBY_MSG_CANCEL:
        if (session->stream.phase != MOBY_STREAM_NONE && msg->u.cancel.stream_id == session->stream.id) {
            stream_stop(session, now_ms, MOBY_AUDIO_END_CANCEL, STOP_DISCARD);
            respond(session, now_ms, id, NULL, NULL);
        } else {
            respond(session, now_ms, id, MOBY_USB_E_STATE, "no such stream");
        }
        break;
    case MOBY_MSG_SET_CONFIG:
        apply_patch(session, &msg->u.set_config);
        respond(session, now_ms, id, NULL, NULL);
        break;
    case MOBY_MSG_PAIR_BEGIN:
        on_pair_begin(session, now_ms, msg);
        break;
    case MOBY_MSG_PAIR_SET_KEY:
        on_pair_set_key(session, now_ms, msg);
        break;
    default:
        respond(session, now_ms, id, MOBY_USB_E_UNSUPPORTED, "unsupported command");
        break;
    }
}

void moby_session_on_message(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg)
{
    if (msg->name == MOBY_MSG_HELLO_ACK) {
        on_hello_ack(session, now_ms, msg);
    } else {
        on_command(session, now_ms, msg);
    }
}

bool moby_session_pop(moby_session_t *session, moby_out_t *out)
{
    if (session->outbox_count == 0) {
        return false;
    }
    *out = session->outbox[session->outbox_head];
    session->outbox_head = (session->outbox_head + 1) % MOBY_OUTBOX_LEN;
    --session->outbox_count;
    return true;
}

void moby_session_view(const moby_session_t *session, moby_face_view_t *view)
{
    memset(view, 0, sizeof *view);
    view->expression = (moby_expression_t)effective(session);
    view->attention = session->attention;
    if (view->expression != MOBY_FACE_ERROR && view->expression != MOBY_FACE_MUTED &&
        view->expression != MOBY_FACE_DISCONNECTED) {
        memcpy(view->text, session->text, sizeof session->text);
    }
}

bool moby_session_recording(const moby_session_t *session)
{
    return session->stream.phase == MOBY_STREAM_CAPTURING;
}

void moby_session_stream_info(const moby_session_t *session, moby_stream_info_t *info)
{
    info->open = session->stream.phase != MOBY_STREAM_NONE;
    info->draining = session->stream.phase == MOBY_STREAM_DRAINING;
    info->stream_id = session->stream.id;
    info->sequence = session->stream.frames;
}

void moby_session_stream_frame_sent(moby_session_t *session)
{
    if (session->stream.phase != MOBY_STREAM_NONE) {
        ++session->stream.frames;
        ++session->stats.stream_frames_sent;
    }
}

void moby_session_stream_drained(moby_session_t *session, uint32_t now_ms)
{
    if (session->stream.phase == MOBY_STREAM_DRAINING) {
        finish_stream(session, now_ms, session->stream.end_reason, true);
        note(session, "stream ended");
    }
}

void moby_session_stream_events(moby_session_t *session, uint32_t now_ms, uint32_t events)
{
    if (session->stream.phase == MOBY_STREAM_NONE) {
        return;
    }
    if (events & MOBY_PCM_EVENT_OVERRUN) {
        ++session->stats.stream_overruns;
        stream_fail(session, now_ms, MOBY_USB_E_AUDIO_OVERRUN, "audio ring overrun");
    } else if (events & MOBY_PCM_EVENT_FAULT) {
        ++session->stats.stream_faults;
        stream_fail(session, now_ms, MOBY_USB_E_HW, "microphone failed");
    }
}

void moby_session_stream_start_lost(moby_session_t *session, uint32_t now_ms)
{
    if (session->stream.phase == MOBY_STREAM_NONE) {
        return;
    }
    ++session->stats.stream_start_lost;
    stream_stop(session, now_ms, MOBY_AUDIO_END_ERROR, STOP_SILENT);
    raise_error(session, now_ms, MOBY_USB_E_AUDIO_OVERRUN, "audio could not start");
}
