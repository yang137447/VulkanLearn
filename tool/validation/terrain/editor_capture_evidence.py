"""Strict R9 capture lifecycle validation; aborted captures never supply measurements."""


def check_runtime_sink(rows):
    failures = []
    pending = {}
    completed = []
    aborted = []
    closed_captures = set()
    previous_time = 0
    closed = False
    for index, row in enumerate(rows, 1):
        if row.get("sequence") != index:
            failures.append("Native sink sequence gap")
        timestamp = row.get("steady_ns")
        if not isinstance(timestamp, int) or timestamp < previous_time:
            failures.append("Native sink timestamp missing or out of order")
        else:
            previous_time = timestamp
        if closed:
            failures.append("Record after native terminal")
        kind = row.get("eventClass")
        payload = row.get("payload", {})
        if not isinstance(payload, dict):
            failures.append("Native sink payload is not an object")
            continue
        if kind == "runtimeEvidenceEnd":
            closed = True
            if payload.get("incomplete") is not False:
                failures.append("Native terminal is incomplete")
            continue
        if kind != "RVT_QUALITY_EVENT":
            continue
        event = payload.get("event")
        if event not in ("capture_submitted", "capture_completed", "capture_aborted"):
            continue
        identity_fields = ("captureHandle", "worldGeneration", "poseId", "frame",
                           "drawSubmissionEpoch", "submissionEpoch")
        if any(type(payload.get(field)) is not int for field in identity_fields):
            failures.append("Capture lacks complete native lifecycle identity")
            continue
        identity = tuple(payload[field] for field in identity_fields)
        if identity in closed_captures:
            failures.append("Capture identity reused after completion/abort")
            continue
        handle, world, pose, frame, draw, submission = identity
        if min(handle, world, pose, draw) <= 0 or frame < 0 or submission < 0:
            failures.append("Capture lifecycle identity is invalid")
            continue
        key = (world, handle)
        if event == "capture_submitted":
            if key in pending or submission != draw or payload.get("submitted") is not True or (
                    payload.get("measurementProvided") is not False):
                failures.append("Capture submission does not bind one actual pending capture")
            else:
                pending[key] = identity
        elif event == "capture_completed":
            if pending.get(key) != identity or payload.get("measurementProvided") is not True or (
                    payload.get("submitted") is not True) or (
                    type(payload.get("completedEpoch")) is not int or payload["completedEpoch"] < submission):
                failures.append("Capture completion has no matching fenced measurement")
            else:
                completed.append(row)
                closed_captures.add(identity)
                del pending[key]
        else:
            reason = payload.get("reason")
            if payload.get("measurementProvided") is not False or payload.get("complete") is True or (
                    payload.get("completedEpoch", 0) != 0) or not isinstance(reason, str) or not reason:
                failures.append("Capture aborted incorrectly claims completion/measurement")
                continue
            if payload.get("submitted") is True:
                if pending.get(key) != identity or submission != draw:
                    failures.append("Capture abort has no matching submitted pending capture")
                    continue
                del pending[key]
            elif payload.get("submitted") is False:
                if submission != 0 or key in pending:
                    failures.append("Unsubmitted capture abort conflicts with a pending submission")
                    continue
            else:
                failures.append("Capture abort lacks native submitted flag")
                continue
            aborted.append(row)
            closed_captures.add(identity)
    if not closed:
        failures.append("Missing native terminal")
    if pending:
        failures.append("Native capture pending at terminal without completion or abort")
    return {
        "failures": failures, "completedMeasurements": completed,
        "abortedWithoutMeasurement": aborted, "pendingCount": len(pending),
        "nativeTerminalComplete": closed and not any("terminal" in failure.lower() for failure in failures)
    }
