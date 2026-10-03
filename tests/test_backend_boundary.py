"""Regression tests of the boundary CLI using real compiler-produced archives."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_backend_boundary.py"


class BoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("gcc") or shutil.which("clang")
        cls.archiver = shutil.which("ar") or shutil.which("llvm-ar")
        if not all((cls.compiler, cls.archiver, shutil.which("nm") or shutil.which("llvm-nm"))):
            raise RuntimeError("Boundary tests need a C compiler, ar, and nm")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.baseline = self.root / "baseline.json"
        self.make_archive("libderen_vulkan.a", "int backend_a(void) { return 1; } int backend_b(void) { return 2; }")
        self.make_archive("libvulkancorekit.a", "extern int backend_a(void); int engine_entry(void) { return backend_a(); }")
        self.write_baseline(["backend_a"])

    def make_archive(self, name, source):
        path = self.root / (name + ".c")
        obj = self.root / (name + ".obj")
        path.write_text(source, encoding="utf-8")
        subprocess.run([self.compiler, "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
        subprocess.run([self.archiver, "rcs", str(self.root / name), str(obj)], check=True, capture_output=True)
        return self.root / name

    def write_baseline(self, symbols):
        self.baseline.write_text(json.dumps({"count": len(symbols), "cross_boundary_symbols": symbols,
                                             "owning_stl_count": 0, "owning_stl_symbols": []}), encoding="utf-8")

    def run_gate(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--build-dir", str(self.root),
                               "--baseline", str(self.baseline), *args], capture_output=True, text=True)

    def test_existing_dependency_passes(self):
        self.assertEqual(self.run_gate().returncode, 0)

    def test_same_count_new_dependency_fails(self):
        self.make_archive("libvulkancorekit.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend_b", result.stdout)

    def test_reduction_does_not_hide_new_dependency(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        self.make_archive("libvulkancorekit.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        self.assertEqual(self.run_gate().returncode, 1)

    def test_growing_update_preserves_baseline_bytes(self):
        self.write_baseline([])
        before = self.baseline.read_bytes()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_replacement_update_is_rejected(self):
        self.make_archive("libvulkancorekit.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_warn_never_accepts_growing_update(self):
        self.write_baseline([])
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--warn", "--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_valid_reduction_can_update(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        self.assertEqual(self.run_gate("--update").returncode, 0)
        self.assertEqual(json.loads(self.baseline.read_text())["cross_boundary_symbols"], ["backend_a"])

    def test_missing_baseline_is_failure(self):
        self.baseline.unlink()
        self.assertEqual(self.run_gate().returncode, 1)

    def test_missing_baseline_requires_explicit_initialization(self):
        self.baseline.unlink()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertFalse(self.baseline.exists())

    def test_initialization_records_first_set(self):
        self.baseline.unlink()
        result = self.run_gate("--initialize")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(self.baseline.read_text())["cross_boundary_symbols"], ["backend_a"])

    def test_initialization_cannot_overwrite_existing_baseline(self):
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--initialize").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_reverse_dependency_is_failure(self):
        self.make_archive("libderen_vulkan.a", "extern int engine_entry(void); int backend_a(void) { return engine_entry(); }")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("engine_entry", result.stdout)

    def test_stale_archive_blocks_update(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        backend_time = (self.root / "libderen_vulkan.a").stat().st_mtime
        os.utime(self.root / "libvulkancorekit.a", (backend_time - 600, backend_time - 600))
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_bad_baseline_is_named_failure(self):
        self.baseline.write_text('{"count":0,"cross_boundary_symbols":["backend_a"]}', encoding="utf-8")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1)
        self.assertIn("baseline", result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_report_exposes_members_and_set_delta(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        report = self.root / "report.json"
        result = self.run_gate("--report", str(report))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text())
        self.assertEqual(data["joiners"], [])
        self.assertEqual(data["leavers"], ["retired_dependency"])
        self.assertEqual(data["reverse_boundary_symbols"], [])
        self.assertEqual(data["count"], 1)
        self.assertEqual(data["usages"], 1)
        self.assertEqual(len(data["consumers"][0]["members"]), 1)

    def test_application_cannot_bypass_archive_measurement(self):
        consumer = self.make_archive("application.a", "extern int backend_b(void); int app_entry(void) { return backend_b(); }")
        result = self.run_gate("--consumer", str(consumer))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend_b", result.stdout)

    def test_nonzero_ratchet_does_not_pass_flip_gate(self):
        result = self.run_gate("--require-zero", "--consumer", str(self.root / "libvulkancorekit.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_zero_gate_requires_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_zero_gate_accepts_clean_explicit_consumer(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        app = self.make_archive("application.a", "int app_entry(void) { return 0; }")
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_duplicate_engine_is_not_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero", "--consumer", str(self.root / "libvulkancorekit.a"))
        self.assertEqual(result.returncode, 1)

    def test_backend_archive_is_not_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero", "--app-object", str(self.root / "libderen_vulkan.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend archive", result.stdout)

    def test_backend_hardlink_is_not_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        alias = self.root / "backend_alias.a"
        os.link(self.root / "libderen_vulkan.a", alias)
        result = self.run_gate("--require-zero", "--app-object", str(alias))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_engine_hardlink_is_not_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        alias = self.root / "engine_alias.a"
        os.link(self.root / "libvulkancorekit.a", alias)
        result = self.run_gate("--require-zero", "--app-object", str(alias))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_backend_cannot_be_added_as_engine_consumer(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        app = self.make_archive("application.a", "int app_entry(void) { return 0; }")
        result = self.run_gate("--require-zero", "--app-object", str(app),
                               "--consumer", str(self.root / "libderen_vulkan.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend archive", result.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows path case aliases")
    def test_case_alias_report_cannot_overwrite_baseline(self):
        before = self.baseline.read_bytes()
        result = self.run_gate("--report", str(self.baseline).upper())
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_chores_without_main_is_not_complete_application_evidence(self):
        self.make_archive("libvulkancorekit.a", "int engine_entry(void) { return 0; }")
        self.make_archive("libchores.a", "int chore_entry(void) { return 0; }")
        self.write_baseline([])
        self.assertEqual(self.run_gate("--require-zero").returncode, 1)

    def test_backend_internal_reference_is_not_a_reverse_dependency(self):
        self.make_archive("libvulkancorekit.a", "extern int backend_a(void); int shared_helper(void) { return 3; } int engine_entry(void) { return backend_a(); }")
        objects = []
        for name, source in (("backend_caller", "extern int shared_helper(void); int backend_a(void) { return shared_helper(); }"),
                             ("backend_provider", "int shared_helper(void) { return 5; }")):
            path = self.root / (name + ".c")
            obj = self.root / (name + ".obj")
            path.write_text(source, encoding="utf-8")
            subprocess.run([self.compiler, "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
            objects.append(str(obj))
        (self.root / "libderen_vulkan.a").unlink()
        subprocess.run([self.archiver, "rcs", str(self.root / "libderen_vulkan.a"), *objects], check=True)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
