import subprocess
import sys

tests = [
    ("Basic Upload/Download", "test_full.py"),
    ("Concurrent Uploads", "test_concurrent_upload.py"),
    ("Concurrent Downloads", "test_concurrent_download.py"),
    ("Deduplication", "test_dedup.py"),
    ("Large File", "test_large_file.py"),
    ("Race Condition Stress", "test_race_condition.py"),
    ("Error Handling", "test_errors.py"),
]

print("=" * 70)
print("FINAL TEST SUITE - OS Midterm Project")
print("=" * 70)

passed = 0
failed = 0

for name, script in tests:
    print(f"\n{'='*70}")
    print(f"Running: {name}")
    print('='*70)

    result = subprocess.run([sys.executable, script],
                            capture_output=False,
                            text=True)

    if result.returncode == 0:
        passed += 1
        print(f"✅ {name} - PASSED")
    else:
        failed += 1
        print(f"❌ {name} - FAILED")

print("\n" + "=" * 70)
print("FINAL RESULTS:")
print(f"  Passed: {passed}/{len(tests)}")
print(f"  Failed: {failed}/{len(tests)}")
print("=" * 70)

if failed == 0:
    print("\n🎉 ALL TESTS PASSED! Project is ready for submission!")
else:
    print(f"\n⚠️ {failed} test(s) failed. Review logs above.")