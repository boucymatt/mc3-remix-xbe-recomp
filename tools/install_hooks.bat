@echo off
rem Turns on the repo's git hooks for this clone: the pre-commit hook runs tools\check_repo_clean.py --staged, which
rem refuses game files, generated code, binaries, captures and secrets. Run it once after cloning.
setlocal
cd /d "%~dp0.."
git rev-parse --is-inside-work-tree >nul 2>nul || (echo This is not a git clone. & exit /b 1)
git config core.hooksPath .githooks || exit /b 1
echo git hooks enabled: .githooks\pre-commit will check every commit.
python tools\test_repo_guard.py >nul 2>nul && (echo repo guard self-test: ok) || (echo WARNING: the repo guard self-test did not pass, is Python 3.9 or newer installed?)
exit /b 0
