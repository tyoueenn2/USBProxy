"""Exercise the built CLI without USB hardware or elevated permissions."""
import subprocess
import sys


def check(arguments, success):
    result = subprocess.run(
        [sys.argv[1], *arguments], capture_output=True, text=True, timeout=5
    )
    assert (result.returncode == 0) == success, result
    return result.stdout + result.stderr


help_text = check(["--help"], True)
assert "--stats" in help_text
assert "--mouse-1000hz" not in help_text
assert "HID::1000Hz" not in help_text
assert check(["-h"], True) == help_text
assert check(["--stats", "--help"], True) == help_text
assert "unrecognized option" in check(["--mouse-1000hz"], False)
assert "unrecognized option" in check(["--invalid-option"], False)
print("CLI tests passed")
