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

        upload_task = main[main.index("void uploadTaskCode(void* pvParameters) {") :]
        send_position = upload_task.index("sendBatch(liveBatch)")
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

    def test_sampler_uses_hardware_timer_without_fake_catch_up(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn("timerBegin(0, 2, true)", main)
        self.assertIn("timerAlarmWrite(samplingTimer, SAMPLE_TIMER_TICKS, true)", main)
        self.assertIn("vTaskNotifyGiveFromISR", main)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", main)
        self.assertIn('"hardware sampling task was delayed"', main)
        self.assertNotIn("lastSampleTime", main)
        self.assertNotIn("micros()", main)

    def test_high_risk_legacy_scripts_are_removed(self) -> None:
        self.assertFalse((FIRMWARE / "flash_and_register.sh").exists())
        self.assertFalse((FIRMWARE / "patch_main.py").exists())

    def test_failed_upload_batch_is_persisted_before_release(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")
        upload_task = main[main.index("void uploadTaskCode(void* pvParameters) {") :]

        self.assertIn("sensorSpool.append(liveBatch)", upload_task)
        self.assertIn("sensorSpool.peek(pending)", upload_task)
        self.assertIn("sensorSpool.acknowledge()", upload_task)
        self.assertLess(
            upload_task.index("sensorSpool.append(liveBatch)"),
            upload_task.index("xQueueSend(freeBatchQueue"),
        )

    def test_batches_publish_bounded_quality_metadata(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")
        sender = main[main.index("bool sendBatch(const SensorBatch& batch)") :]

        for required in (
            'doc["protocol_version"] = batch.protocolVersion;',
            'doc["firmware_version"] = batch.firmwareVersion;',
            'doc["calibration_version"] = batch.calibrationVersion;',
            'doc["boot_id"] = batch.bootId;',
            'doc["quality_counts"]',
            'quality["valid"]',
            'quality["lead_off"]',
            'quality["rail_high"]',
            'quality["rail_low"]',
            'quality["jump"]',
            'quality["recovery"]',
            'doc["values_deci_mv"]',
        ):
            self.assertIn(required, sender)

    def test_batch_reports_sequence_uptime_and_dropped_samples(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")

        for required in (
            "activeBatch.sequence = nextBatchSequence++;",
            "activeBatch.uptimeMs = millis();",
            "activeBatch.droppedSamplesTotal = droppedSampleCount;",
            'doc["sequence"] = batch.sequence;',
            'doc["uptime_ms"] = batch.uptimeMs;',
            'doc["dropped_samples_total"] = batch.droppedSamplesTotal;',
        ):
            self.assertIn(required, main)

    def test_n16r8_partition_retains_ota_and_large_spool(self) -> None:
        partitions = (FIRMWARE / "partitions.csv").read_text(encoding="utf-8")
        platformio = (FIRMWARE / "platformio.ini").read_text(encoding="utf-8")
        board = (FIRMWARE / "boards" / "biolingo_v22.json").read_text(encoding="utf-8")

        self.assertIn("app0,     app,  ota_0,   0x020000,  0x300000", partitions)
        self.assertIn("app1,     app,  ota_1,   0x320000,  0x300000", partitions)
        self.assertIn("spiffs,   data, spiffs,  0x620000,  0x9D0000", partitions)
        self.assertIn("board = biolingo_v22", platformio)
        self.assertIn('"flash_size": "16MB"', board)
        self.assertIn('"memory_type": "qio_opi"', board)

    def test_spool_records_are_crc_protected_and_never_auto_reformatted(self) -> None:
        spool = (FIRMWARE / "src" / "sensor_spool.cpp").read_text(encoding="utf-8")

        self.assertIn("uint32_t crc32", spool)
        self.assertIn("expected != record.crc32", spool)
        self.assertIn("LittleFS.begin(false)", spool)
        self.assertNotIn("LittleFS.begin(true)", spool)
        self.assertIn("retained without formatting", spool)

    def test_registration_retries_are_bounded_and_pairing_is_retained(self) -> None:
        main = (FIRMWARE / "src" / "main.cpp").read_text(encoding="utf-8")
        registration = main[main.index("void registrationTaskCode(void*") :]

        self.assertIn("MAX_REGISTRATION_ATTEMPTS = 5", main)
        self.assertIn("REGISTRATION_RETRY_INTERVAL_MS = 60000", main)
        self.assertIn("attempt <= MAX_REGISTRATION_ATTEMPTS", registration)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(REGISTRATION_RETRY_INTERVAL_MS))", registration)
        self.assertIn("bool permanentFailure", registration)
        self.assertNotIn('pairingCode = "";\n        saveConfig();\n    }', main)


if __name__ == "__main__":
    unittest.main()
