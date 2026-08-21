"""Focused regression checks for firmware security-critical source invariants."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "GreenMindFirmware_Biolingo"


class FirmwareSecurityInvariantTests(unittest.TestCase):
    def test_build_does_not_embed_wifi_credentials(self) -> None:
        platformio = (FIRMWARE / "platformio.ini").read_text(encoding="utf-8")
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        for forbidden in ("PROVISION_SSID", "PROVISION_PASS"):
            self.assertNotIn(forbidden, platformio)
            self.assertNotIn(forbidden, main)

        self.assertNotIn("pairingCode.c_str()", main)
        self.assertNotIn('Code: %s', main)
        self.assertIn("esp_random()", main)

    def test_open_wifi_network_counts_as_provisioned(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn("isProvisioned = (wifiSSID.length() > 0);", main)
        self.assertNotIn("wifiSSID.length() > 0 && wifiPass.length() > 0", main)

    def test_batch_ownership_returns_only_after_upload(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn("freeBatchQueue", main)
        self.assertIn("sizeof(uint8_t)", main)
        self.assertNotIn("sizeof(SensorBatch*)", main)
        self.assertNotIn("static SensorBatch   buffers[2]", main)

        upload_task = main[main.index("void uploadTaskCode(") :]
        send_position = upload_task.index("sendBatch(batchPool[batchToUpload])")
        release_position = upload_task.index(
            "xQueueSend(freeBatchQueue, &batchToUpload, portMAX_DELAY)"
        )
        self.assertLess(send_position, release_position)

    def test_ota_hash_verification_precedes_partition_finalize(self) -> None:
        ota = (FIRMWARE / "src" / "ota_client.cpp").read_text(encoding="utf-8")
        download = ota[ota.index("bool GreenMindOTA::performDownload") :]

        for required in (
            "parseSha256Hex(expectedSha256, expectedDigest)",
            "ESP.getFreeSketchSpace()",
            "mbedtls_sha256_update_ret",
            "mbedtls_sha256_finish_ret",
            "constantTimeDigestEquals(actualDigest, expectedDigest)",
            "Update.abort()",
        ):
            self.assertIn(required, download)

        verify_position = download.index(
            "constantTimeDigestEquals(actualDigest, expectedDigest)"
        )
        finalize_position = download.index("Update.end()")
        self.assertLess(verify_position, finalize_position)

    def test_sampler_records_blocking_gaps_without_fake_catch_up(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")
        sampler_start = main.index("void streamReadings() {")
        sampler_end = main.index("void uploadTaskCode(void*", sampler_start)
        sampler = main[sampler_start:sampler_end]

        gap_position = sampler.index(
            'recordDroppedSamples(missedSamples + partialBatchSamples, "sampler was blocked")'
        )
        partial_reset_position = sampler.index("bufferIndex = 0;")
        reset_position = sampler.index("lastSampleTime = now;")
        adc_position = sampler.index("analogRead(ADC_PIN)")
        self.assertLess(gap_position, adc_position)
        self.assertLess(partial_reset_position, adc_position)
        self.assertLess(reset_position, adc_position)
        self.assertNotIn("lastSampleTime += SAMPLE_INTERVAL_US; // Exact timing compensation", sampler)

    def test_high_risk_legacy_scripts_are_removed(self) -> None:
        self.assertFalse((FIRMWARE / "flash_and_register.sh").exists())
        self.assertFalse((FIRMWARE / "patch_main.py").exists())

    def test_failed_upload_batch_is_counted_as_dropped(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn(
            'recordDroppedSamples(BATCH_SIZE, "gateway did not acknowledge batch")',
            main,
        )


if __name__ == "__main__":
    unittest.main()
