import base64
import hashlib
import json
import unittest
import zlib

from tools import garmin_provision


FAKE_FIELDS = {
    "kind": "pikos.garmin.di",
    "schema": 1,
    "domain": "garmin.com",
    "client_id": "TEST_CLIENT_ID",
    "refresh_token": "NOT_A_REAL_TOKEN",
}


class GarminProvisionEnvelopeTests(unittest.TestCase):
    def test_real_provisioning_stays_release_disabled(self) -> None:
        self.assertFalse(garmin_provision.PROVISIONING_ENABLED)

    def test_fake_envelope_has_exact_fields_and_checksums(self) -> None:
        wire, digest = garmin_provision.make_wire_envelope(FAKE_FIELDS, "00" * 16)
        envelope = json.loads(wire[len(garmin_provision.DATA_PREFIX) :])
        payload = base64.b64decode(envelope["payload_b64"], validate=True)
        decoded = json.loads(payload)
        self.assertEqual(set(decoded), set(FAKE_FIELDS))
        self.assertNotIn("di_token", decoded)
        self.assertNotIn("email", decoded)
        self.assertNotIn("password", decoded)
        self.assertEqual(envelope["payload_len"], len(payload))
        self.assertEqual(envelope["sha256"], digest)
        self.assertEqual(digest, hashlib.sha256(payload).hexdigest())
        self.assertEqual(envelope["crc32"], f"{zlib.crc32(payload) & 0xFFFFFFFF:08x}")
        wire[:] = b"\x00" * len(wire)

    def test_extra_field_control_character_and_bad_nonce_are_rejected(self) -> None:
        with self.assertRaises(garmin_provision.ProvisioningError):
            garmin_provision.canonical_payload({**FAKE_FIELDS, "email": "fixture@example.test"})
        with self.assertRaises(garmin_provision.ProvisioningError):
            garmin_provision.canonical_payload({**FAKE_FIELDS, "client_id": "bad\nvalue"})
        with self.assertRaises(garmin_provision.ProvisioningError):
            garmin_provision.make_wire_envelope(FAKE_FIELDS, "not-a-nonce")


if __name__ == "__main__":
    unittest.main()
