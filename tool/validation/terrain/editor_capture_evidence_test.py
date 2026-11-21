import copy
import unittest
from editor_capture_evidence import check_runtime_sink


class CaptureEvidenceTests(unittest.TestCase):
    def capture(self, event, submitted=True):
        return {
            "eventClass": "RVT_QUALITY_EVENT", "payload": {
                "event": event, "captureHandle": 9, "worldGeneration": 1, "poseId": 2,
                "frame": 10, "drawSubmissionEpoch": 11, "submissionEpoch": 11 if submitted else 0,
                "submitted": submitted, "measurementProvided": event == "capture_completed"
            }
        }

    def sink(self, events, incomplete=False):
        rows = copy.deepcopy(events) + [{"eventClass": "runtimeEvidenceEnd", "payload": {"incomplete": incomplete}}]
        for index, row in enumerate(rows, 1):
            row.update(sequence=index, steady_ns=index * 1000)
        return check_runtime_sink(rows)

    def test_matching_completion_is_measurement(self):
        completed = self.capture("capture_completed")
        completed["payload"]["completedEpoch"] = 11
        result = self.sink([self.capture("capture_submitted"), completed])
        self.assertFalse(result["failures"])
        self.assertEqual(len(result["completedMeasurements"]), 1)

    def test_submitted_abort_closes_only_pending(self):
        aborted = self.capture("capture_aborted")
        aborted["payload"]["reason"] = "shutdown"
        result = self.sink([self.capture("capture_submitted"), aborted])
        self.assertFalse(result["failures"])
        self.assertEqual(len(result["completedMeasurements"]), 0)
        self.assertEqual(len(result["abortedWithoutMeasurement"]), 1)

    def test_recorded_unsubmitted_abort_is_not_measurement(self):
        aborted = self.capture("capture_aborted", False)
        aborted["payload"]["reason"] = "frame_aborted"
        result = self.sink([aborted])
        self.assertFalse(result["failures"])
        self.assertEqual(len(result["completedMeasurements"]), 0)

    def test_abort_cannot_claim_measurement(self):
        aborted = self.capture("capture_aborted")
        aborted["payload"].update(reason="shutdown", measurementProvided=True)
        self.assertTrue(self.sink([self.capture("capture_submitted"), aborted])["failures"])

    def test_abort_cannot_claim_complete_epoch(self):
        aborted = self.capture("capture_aborted")
        aborted["payload"].update(reason="shutdown", completedEpoch=11)
        self.assertTrue(self.sink([self.capture("capture_submitted"), aborted])["failures"])

    def test_completion_after_abort_rejected(self):
        aborted = self.capture("capture_aborted")
        aborted["payload"]["reason"] = "world_replaced"
        completed = self.capture("capture_completed")
        completed["payload"]["completedEpoch"] = 11
        self.assertTrue(self.sink([self.capture("capture_submitted"), aborted, completed])["failures"])

    def test_mismatched_capture_identity_rejected(self):
        completed = self.capture("capture_completed")
        completed["payload"].update(frame=12, completedEpoch=11)
        self.assertTrue(self.sink([self.capture("capture_submitted"), completed])["failures"])

    def test_pending_capture_cannot_pass_complete_footer(self):
        self.assertTrue(self.sink([self.capture("capture_submitted")])["failures"])

    def test_incomplete_footer_rejected(self):
        self.assertTrue(self.sink([], True)["failures"])

    def test_missing_identity_rejected(self):
        aborted = self.capture("capture_aborted")
        aborted["payload"].update(reason="shutdown")
        del aborted["payload"]["captureHandle"]
        self.assertTrue(self.sink([aborted])["failures"])

    def test_duplicate_recorded_abort_rejected(self):
        aborted = self.capture("capture_aborted", False)
        aborted["payload"]["reason"] = "shutdown"
        self.assertTrue(self.sink([aborted, aborted])["failures"])

    def test_nonobject_payload_rejected(self):
        self.assertTrue(self.sink([{"eventClass": "RVT_QUALITY_EVENT", "payload": []}])["failures"])


if __name__ == "__main__":
    unittest.main()
