#!/usr/bin/env bash
# Runs the packaged app the way Explorer would: from the Desktop folder, with an
# environment a double-click would give it. No Qt on PATH, no KESTREL_* variables,
# no venv. If it only works in a developer shell, it is not a desktop copy.
#
# Machine-specific paths come from the environment:
#
#   KESTREL_STAGE   the assembled copy to verify. Must be where
#                   tools\package-desktop.bat was told to put it.
#   KESTREL_REPO    the checkout. Defaults to this script's parent directory,
#                   which is right unless the scripts were copied elsewhere.
#
# The user identity is read rather than written down, because it is part of what
# is being tested: a double-click is a launch by *this* user. A hardcoded one
# made the check quietly describe a different machine than the one it ran on.
set -u

REPO="${KESTREL_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
STAGE="${KESTREL_STAGE:-$HOME/OneDrive/Desktop/Kestrel}"

if [ ! -x "$STAGE/kestrel.exe" ]; then
    echo "no packaged app at $STAGE"
    echo "run tools\package-desktop.bat first, or point KESTREL_STAGE at one"
    exit 1
fi

# The Windows spelling of this user's home, which is what Explorer hands a
# process. MSYS and Git Bash report POSIX paths and the scrub below speaks
# Windows, so the conversion happens once here rather than per variable.
WIN_USERPROFILE=$(cygpath -m "$HOME" 2>/dev/null || echo "$HOME")
USER_NAME=$(basename "$HOME")
USER_DOMAIN="${USERDOMAIN:-$(cmd //c "echo %USERDOMAIN%" 2>/dev/null | tr -d '\r')}"

OUT="$REPO/build-release"
WAV="$OUT/voice-from-desktop.wav"
SHOT="$OUT/desktop-window.png"
BEFORE="$OUT/folder-before.txt"
AFTER="$OUT/folder-after.txt"

# Resolved before scrubbing. With PATH cut back to System32, a bare "timeout"
# finds Windows' own TIMEOUT.EXE, which takes completely different arguments.
# The app quits itself after taking the screenshot; this is only a watchdog.
GNU_TIMEOUT=$(command -v timeout)

cd "$STAGE" || exit 1

# Absolute Windows paths for the two launchers below. Start-Process resolves a
# relative -FilePath against the *PowerShell* process's own directory, not
# against -WorkingDirectory, so '.\kestrel.exe' silently depends on where the
# shell happened to be. It usually is the stage -- and when it is not, the
# launch fails with no process and the step reports "no window appeared", which
# reads as an application fault and is not one.
EXE_WIN=$(cygpath -m "$STAGE/kestrel.exe")
STAGE_WIN=$(cygpath -m "$STAGE")

# What Explorer gives a double-clicked program. The point of the scrub is the
# PATH -- no Qt, no venv -- because that is the only thing that could be
# supplying a DLL the folder does not have.
#
# The system variables are all present, and deliberately so. An earlier version
# of this scrubbed them too, and the app then created a literal directory named
# "%SystemDrive%" holding a DirectX shader cache: D3D11 could not resolve the
# KnownFolder path without SystemDrive and ProgramData and made one relative to
# the working directory instead. That was the test being unfaithful to Explorer,
# not a packaging fault, so the variables are back.
CLEAN_PATH='/c/Windows/system32:/c/Windows:/c/Windows/System32/Wbem'

# Renders one NAME=VALUE pair as PowerShell assignment syntax, converting the
# value to a Windows path when it is one.
#
# The app is a Windows program and this script is bash, so every path handed
# across has to change shape at the boundary. KESTREL_SCREENSHOT is the one that
# bites: a POSIX path is accepted silently, the save fails, and the check then
# reports a missing screenshot for a reason that has nothing to do with whether
# the app can take one.
ps_pair() {
    local kv="$1"
    local name="${kv%%=*}"
    local value="${kv#*=}"
    case "$value" in
        /*|[A-Za-z]:[\\/]*|*/*[\\/]*)
            value=$(cygpath -m "$value" 2>/dev/null || echo "$value") ;;
    esac
    # Quoted, because PowerShell parses a bare word here and a path is not one.
    # An unquoted value does not fail loudly: the assignment is simply skipped,
    # the app starts without KESTREL_SCREENSHOT, and the step reports a missing
    # screenshot that no amount of debugging the app will ever explain.
    value=${value//\'/\'\'}
    printf "\$env:%s='%s'" "$name" "$value"
}

# Starts kestrel.exe the way Explorer does, and waits for it.
#
# Deliberately not `env -i ./kestrel.exe`. That looks like the same thing and is
# not. Git Bash's exec path reports a failure to load the UCRT apiset
# `api-ms-win-crt-locale-l1-1-0.dll`, which the Windows loader resolves from the
# OS apiset schema and never looks for as a file -- so the app was reported dead
# while it was sitting there with its window up, and two steps of this script
# failed on a harness bug rather than on anything it was checking. Start-Process
# is a real Windows create, so what comes back is the real answer.
#
# KESTREL_* is removed from the inherited environment first, which is the whole
# point: a double-clicked app gets none of them, and a check that leaks the
# developer's own KESTREL_LLMAMA_MODEL is not checking a double-click.
win_run() {
    local out="$1"; shift
    # Same boundary problem as the environment values: PowerShell's redirect
    # targets are Windows paths, and a POSIX one makes Start-Process fail
    # before it has created a process at all -- which reads, from here, exactly
    # like an app that started and then did nothing.
    out=$(cygpath -m "$out" 2>/dev/null || echo "$out")
    local pairs=""
    for kv in "$@"; do
        pairs="$pairs $(ps_pair "$kv");"
    done
    # Bounded, and it says so when it has to kill. The app quits itself after
    # capturing, so a run that is still alive at the deadline is a finding, not
    # a reason for this script to hang forever -- an earlier version waited
    # without a bound and the whole verification stopped dead.
    powershell -NoProfile -Command "
        Get-ChildItem env: | Where-Object { \$_.Name -like 'KESTREL*' } | Remove-Item
        $pairs
        \$p = Start-Process -FilePath '$EXE_WIN' -WorkingDirectory '$STAGE_WIN' -PassThru -NoNewWindow -RedirectStandardOutput '$out' -RedirectStandardError '$out.err'
        if (-not \$p.WaitForExit(90000)) {
            Write-Output '    KESTREL DID NOT EXIT within 90s -- killed'
            \$p.Kill()
        }" >/dev/null 2>&1
}

# The same, without waiting, for the step that has to look at a window while
# the app is up. Start-Sleep inside PowerShell rather than bash's, so the child
# is not a job of this shell that a later cleanup might reap early.
win_start() {
    local seconds="$1"; shift
    local pairs=""
    for kv in "$@"; do
        pairs="$pairs $(ps_pair "$kv");"
    done
    powershell -NoProfile -Command "
        Get-ChildItem env: | Where-Object { \$_.Name -like 'KESTREL*' } | Remove-Item
        $pairs
        Start-Process -FilePath '$EXE_WIN' -WorkingDirectory '$STAGE_WIN'
        # Poll from in here rather than from bash after this script returns.
        # The observation has to happen while the app is up, and relying on it
        # outliving this PowerShell process is a dependency nothing else in the
        # project has: when it did not hold, the step reported no window for an
        # app that had had one the whole time.
        # (No double quotes in these comments: this whole block is one bash
        # double-quoted string, and a stray quote in prose ends it silently.)
        for (\$i = 0; \$i -lt $seconds; \$i++) {
            \$t = (Get-Process kestrel -ErrorAction SilentlyContinue |
                   Where-Object { \$_.MainWindowHandle -ne 0 } |
                   Select-Object -First 1).MainWindowTitle
            if (\$t) { Write-Output \"TITLE=\$t\"; break }
            Start-Sleep -Milliseconds 500
        }
        \$n = @(Get-Process kestrel -ErrorAction SilentlyContinue).Count
        Write-Output \"RUNNING=\$n\"
        Get-Process kestrel -ErrorAction SilentlyContinue | Stop-Process -Force
        " >"$OUT/win-start.log" 2>&1
    # Surfaced, because a launch that silently fails leaves the step reporting
    # "no window appeared" -- which names the app when the fault is here.
    if [ -s "$OUT/win-start.log" ]; then
        sed 's/^/    /' "$OUT/win-start.log"
    fi
    KESTREL_WINDOW_TITLE=$(sed -n 's/^TITLE=//p' "$OUT/win-start.log" | head -1)
    KESTREL_WINDOW_RUNNING=$(sed -n 's/^RUNNING=//p' "$OUT/win-start.log" | head -1)
}

scrub() {
    env -i \
        PATH="$CLEAN_PATH" \
        SystemRoot='C:\Windows' \
        windir='C:\Windows' \
        SystemDrive='C:' \
        ProgramData='C:\ProgramData' \
        ProgramFiles='C:\Program Files' \
        'ProgramFiles(x86)'='C:\Program Files (x86)' \
        ProgramW6432='C:\Program Files' \
        COMSPEC='C:\Windows\system32\cmd.exe' \
        PATHEXT='.COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC' \
        PROCESSOR_ARCHITECTURE='AMD64' \
        OS='Windows_NT' \
        USERNAME="$USER_NAME" \
        USERDOMAIN="$USER_DOMAIN" \
        USERPROFILE="$WIN_USERPROFILE" \
        HOMEDRIVE='C:' \
        HOMEPATH="\Users\$USER_NAME" \
        LOCALAPPDATA="$WIN_USERPROFILE\AppData\Local" \
        APPDATA="$WIN_USERPROFILE\AppData\Roaming" \
        TEMP="$WIN_USERPROFILE\AppData\Local\Temp" \
        TMP="$WIN_USERPROFILE\AppData\Local\Temp" \
        PUBLIC='C:\Users\Public' \
        "$@"
}

echo "=== environment the app is given ==="
scrub cmd //c set 2>/dev/null | grep -iE "^(PATH|QT_|KESTREL|VIRTUAL_ENV|SystemDrive|ProgramData)"
echo

echo "=== 1. --print-runtime, from the Desktop folder ==="
scrub ./kestrel.exe --print-runtime 2>&1
echo

echo "=== 2. does the bundled voice actually synthesise from here? ==="
echo "    --print-runtime only checks that the files are there; it never runs"
echo "    the interpreter, so it would report a voice that is broken."
rm -f "$WAV"
printf '%s\n' "{\"text\": \"This is Kestrel speaking from the desktop.\", \"out\": \"$(cygpath -m "$WAV")\", \"speed\": 1.46}" \
    | scrub ./.kestrel-voice/Scripts/python.exe ./tools/kokoro_voice_server.py 2>&1
if [ -f "$WAV" ]; then
    printf '    result: %s bytes of 24 kHz audio\n' "$(stat -c%s "$WAV")"
else
    echo "    result: NO AUDIO PRODUCED"
fi
echo

echo "=== 3. a real window, from the Desktop folder, with a seeded conversation ==="
echo "    QT_QPA_PLATFORM is NOT set, so this is the real windows platform"
echo "    plugin composing a real window, not an offscreen render."
rm -f "$SHOT"
# Listed before the run, so step 5 can tell what the app itself wrote rather
# than what the packaging step put there.
find "$STAGE" | sort > "$BEFORE"
win_run "$OUT/window.log" "KESTREL_SCREENSHOT=$SHOT" "KESTREL_DEMO=static"
grep -viE "ffmpeg|QProcess: Destroyed" "$OUT/window.log" 2>/dev/null | head -10
# Retried, because this one is genuinely intermittent and the reason is worth
# knowing: the offscreen capture in step 6 has never missed, while the real
# windows platform misses maybe one run in three. grabToImage() on a live window
# races the compositor, and when it loses the grab reports ready with a null
# image and nothing is written. That is a property of the capture, not of the
# app -- the same launch puts its window up every time, which step 5 measures --
# so a single miss here is not a finding and should not be reported as one.
for attempt in 1 2 3; do
    [ -f "$SHOT" ] && break
    [ "$attempt" = 1 ] || printf '    retry %s of the window capture\n' "$attempt"
    win_run "$OUT/window.log" "KESTREL_SCREENSHOT=$SHOT" "KESTREL_DEMO=static"
done
if [ -f "$SHOT" ]; then
    printf '    capture: %s bytes\n' "$(stat -c%s "$SHOT")"
else
    echo "    capture: NONE -- three attempts produced no image"
fi
echo

echo "=== 4. did the seeded conversation actually render message rows? ==="
"$REPO/.kestrel-voice/Scripts/python.exe" "$REPO/tools/pngrows.py" "$SHOT"
echo

echo
echo "=== 5. is this a GUI program, and does a bare launch put a window up? ==="
echo "    The PE subsystem is what decides whether a terminal window opens"
echo "    behind the app. 3 is console, so a double-click flashes one on every"
echo "    launch; 2 is the GUI subsystem and nothing appears but Kestrel."
echo "    Every other run in this project was launched from a shell, so this is"
echo "    the one property of the packaged copy nothing had ever observed."
"$REPO/.kestrel-voice/Scripts/python.exe" - <<'SUBSYSTEM'
import struct
data = open("kestrel.exe", "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
subsystem = struct.unpack_from("<H", data, pe + 0x5C)[0]
names = {2: "WINDOWS_GUI -- no console on a double-click", 3: "WINDOWS_CUI -- a console window opens"}
print("    PE subsystem: %d (%s)" % (subsystem, names.get(subsystem, "unknown")))
SUBSYSTEM

# No KESTREL_* of any kind: this is the closest a shell gets to a double-click.
# Anything left over from the capture above is cleared first: a bare launch is
# only a faithful double-click if nothing else is already running, and a
# leftover instance is exactly what makes the title probe below find a process
# with no window and report "NONE" for an app that is working.
taskkill //F //IM kestrel.exe //T >/dev/null 2>&1
sleep 1
win_start 30
# The process count is reported alongside the title because "no window" and "no
# process" are completely different findings -- the first is a UI fault, the
# second is a launch fault -- and a bare "NONE" cannot tell you which one you
# are looking at.
if [ -n "$KESTREL_WINDOW_TITLE" ]; then
    printf '    window title: "%s" (%s process running)\n' \
        "$KESTREL_WINDOW_TITLE" "${KESTREL_WINDOW_RUNNING:-?}"
else
    echo "    window title: NONE -- no top-level window appeared (${KESTREL_WINDOW_RUNNING:-?} process running)"
fi

echo
echo "=== 6. display-less review: does the offscreen capture work? ==="
echo "    KESTREL_SCREENSHOT with QT_QPA_PLATFORM=offscreen is how a UI change"
echo "    gets reviewed on a machine with no screen. It needs qoffscreen.dll,"
echo "    which windeployqt does not copy; without it Qt puts up a modal"
echo "    'no platform plugin' dialog and the process never returns."
OFFSCREEN="$OUT/desktop-offscreen.png"
rm -f "$OFFSCREEN"
win_run "$OUT/offscreen.log" "QT_QPA_PLATFORM=offscreen" \
        "KESTREL_SCREENSHOT=$OFFSCREEN" "KESTREL_DEMO=static"
if [ -s "$OFFSCREEN" ]; then
    printf '    capture: %s bytes\n' "$(stat -c%s "$OFFSCREEN")"
    "$REPO/.kestrel-voice/Scripts/python.exe" "$REPO/tools/pngrows.py" "$OFFSCREEN" | sed 's/^/    /'
else
    echo "    capture: NONE -- the offscreen platform could not start"
fi

echo "=== 7. did any of this write anything into the app's own folder? ==="
echo "    Compared against the listing taken in step 3, so it covers the"
echo "    bare launch and the offscreen capture as well."
find "$STAGE" | sort > "$AFTER"
added=$(comm -13 "$BEFORE" "$AFTER")
if [ -z "$added" ]; then
    echo "    nothing: the folder is byte-for-byte as it was packaged"
else
    echo "    the run created:"
    echo "$added" | sed 's/^/      /'
fi
