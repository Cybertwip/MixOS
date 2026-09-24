package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
)

// The live debug console: the host end of mvii_debug_console.c.
//
// WHY THIS EXISTS RATHER THAN MORE BROM COMMANDS
// The BROM's USB transport belongs to the BROM and dies when a clock it depends
// on is reprogrammed, which on this board means every command that actually
// touches hardware is one that might not survive to report its own result. This
// talks instead to a USB device the firmware owns (mt6592_usb_gadget.c), served
// by an LK that has already run: real clocks, real PMIC, real keypad.
//
// Two modes, and the batch mode is the point of the whole exercise. Interactive
// (-dbg) is a terminal for poking at things. Batch (-dbg-run "a;b;c") runs a
// whole list of commands and prints one transcript, so a diagnosis that needs
// six register dumps costs one round trip with the hardware instead of six.
const (
	mviiDebugConsolePID = 0x4d56 // 'MV'

	// The device does frame its output, and it did all along: mvii_debug_console.c
	// writes "> " after every command it finishes. That two-byte prompt is the
	// end-of-command sentinel, so the host does not have to guess from silence.
	//
	// Guessing from silence was actively wrong for the commands that matter most.
	// `kpdmon` sits waiting for a human to press a button and `ledscan` parks two
	// seconds on each channel; both go quiet for far longer than any gap short
	// enough to be useful between ordinary commands, so the old 400 ms rule cut
	// them off at the first pause and reported the fragment as the whole answer.
	mviiDebugPrompt = "> "

	// Fallback for a device that has not shown a prompt yet -- an older firmware,
	// or output that arrives before the first prompt of the session. Long enough
	// that a human pause does not look like an ending.
	mviiDebugQuietGap = 15 * time.Second

	// Longest a single command may take before the host gives up on it. This is a
	// runaway guard, not a pacing knob: with prompt framing the normal command
	// returns the instant the device is done, so the budget only ever expires on
	// something genuinely stuck. `kpdmon` sessions are driven by hand and can
	// legitimately run for minutes. Override with MVII_DBG_BUDGET.
	mviiDebugCommandBudget = 10 * time.Minute

	// The bridge magic. Both ends name themselves in-band, on top of VID:PID.
	//
	// The device announces mviiDebugDeviceMagic on every attach, including
	// re-attaches after a dropped link, and answers the host's hello with it.
	// The hello is also a RESYNC: the device checks for it anywhere in the line
	// rather than only at the start, so it recovers a command buffer left
	// holding a half-typed fragment by a cable that came out mid-command.
	//
	// Neither end insists. An older LK answers the hello with "unknown command"
	// and works fine; this host prints one note and carries on. The exchange
	// exists to make a MISIDENTIFIED link say so immediately -- the failure it
	// was written for is `flash` without -dbg grabbing the console because both
	// devices are 0x0e8d -- not to gate the protocol.
	mviiDebugHostMagic   = "!MVIIDBG1"
	mviiDebugDeviceMagic = "MVIIDBG1"

	// How long to wait for the device to come back after the link drops mid
	// session, before giving up on it. Matches MVII_DEBUG_CONSOLE_REATTACH_MS in
	// mvii_debug_console.h: on the far side the bridge stays up for five minutes
	// waiting for exactly this, and a host that stopped looking first would make
	// that patience useless.
	mviiDebugReattachWait = 5 * time.Minute
)

var errMVIIDebugNotFound = errors.New("no MVII debug console found (0x0e8d:0x4d56) — power-cycle the board while holding down any face or shoulder button; the screen turns magenta when the console is up and waiting, then green once the host attaches")

// mviiDebugPort is the transport the console needs, which is a strict subset of
// what mtkUSBPort already provides. Naming it separately keeps this file free of
// cgo so it compiles everywhere, with the platform files supplying the opener.
type mviiDebugPort interface {
	ReadSome(buf []byte, timeout time.Duration) (int, error)
	WriteAll(data []byte, timeout time.Duration) error
	Close() error
}

// drainMVIIDebug prints everything the device says, as it says it, until the
// device writes its prompt back (command finished), the link goes quiet for
// mviiDebugQuietGap without ever having shown a prompt, or the budget runs out.
// Returns the transcript.
//
// Every byte is echoed the moment it arrives rather than at the end, so a
// command that takes forty seconds is watchable for all forty of them. That is
// the point of the prompt sentinel: before it, "watchable" and "complete" were
// in conflict, because the only way to know a slow command had finished was to
// wait out a gap long enough to swallow the interesting part.
func drainMVIIDebug(port mviiDebugPort, budget time.Duration, echo bool) (string, error) {
	var sb strings.Builder
	buf := make([]byte, 4096)
	deadline := time.Now().Add(budget)
	lastData := time.Now()

	for {
		n, err := port.ReadSome(buf, 100*time.Millisecond)
		if n > 0 {
			chunk := string(buf[:n])
			sb.WriteString(chunk)
			if echo {
				fmt.Print(chunk)
			}
			if strings.HasSuffix(sb.String(), mviiDebugPrompt) {
				return sb.String(), nil
			}
			lastData = time.Now()
			continue
		}
		if err != nil {
			return sb.String(), err
		}
		if time.Since(lastData) >= mviiDebugQuietGap && sb.Len() > 0 {
			return sb.String(), nil
		}
		if time.Now().After(deadline) {
			if sb.Len() == 0 {
				return "", fmt.Errorf("device sent nothing within %s", budget)
			}
			return sb.String(), nil
		}
	}
}

func sendMVIIDebugLine(port mviiDebugPort, line string) error {
	return port.WriteAll([]byte(line+"\n"), 5*time.Second)
}

// syncMVIIDebug opens (or reopens) a conversation: send the host magic, read
// until the prompt, and report whether the thing on the other end named itself
// as the console.
//
// This replaced a bare empty line, which did the framing job -- the dispatcher
// ignores an empty command and answers with a prompt -- and nothing else. An
// empty line looks exactly the same coming back from a live console, from an
// older LK, and from a device that is not our console at all, so a session that
// opened onto the wrong port produced its first confusing output several
// commands later. The hello costs the same round trip and names the far end.
//
// A false return is a note, not a failure. The console works without the magic;
// what the caller loses is the ability to say so.
func syncMVIIDebug(port mviiDebugPort, budget time.Duration, echo bool) (bool, error) {
	if err := sendMVIIDebugLine(port, mviiDebugHostMagic); err != nil {
		return false, fmt.Errorf("probe the console: %w", err)
	}
	out, err := drainMVIIDebug(port, budget, echo)
	if err != nil && out == "" {
		return false, err
	}
	named := strings.Contains(out, mviiDebugDeviceMagic)

	// Then soak up anything that was already in flight, and get the prompt
	// framing back in step.
	//
	// The device prints its attach banner -- magic, greeting, and on the first
	// attach the whole of `help` -- the instant the host configures it, which is
	// usually before this process opens the port but can equally be a beat
	// after. That banner ends in a prompt of its own, so the drain above may
	// have returned on THAT prompt with the answer to our hello still queued
	// behind it. One prompt out of step is not visibly wrong here; it is
	// visibly wrong three commands later, when a drain ends on the leftover and
	// reports the front of one command's output as the whole of another's.
	//
	// Bounded at four passes so a device that is genuinely streaming (a `kpdmon`
	// left running across a reconnect) cannot hold the sync open.
	for i := 0; i < 4; i++ {
		extra, extraErr := drainMVIIDebug(port, 300*time.Millisecond, echo)
		if extra == "" || extraErr != nil {
			break
		}
		if strings.Contains(extra, mviiDebugDeviceMagic) {
			named = true
		}
	}
	return named, nil
}

// runMVIIDebugConsole attaches to the live console. When script is non-empty it
// runs the semicolon-separated commands and exits; otherwise it is interactive.
func runMVIIDebugConsole(script string) error {
	port, err := openMVIIDebugPort()
	if err != nil {
		return err
	}
	defer port.Close()

	fmt.Println("MVII live debug console attached (0x0e8d:0x4d56).")

	// Synchronize on the prompt before doing anything else. The firmware prints
	// its banner and help as soon as it is configured, which is usually long
	// before this process starts, so there may be nothing queued at all and
	// waiting for a banner just burns the timeout. The hello is the cheap way to
	// find out: it proves the link is alive, leaves the stream at a known place
	// for the first real command, and names the far end.
	named, err := syncMVIIDebug(port, 5*time.Second, true)
	if err != nil {
		fmt.Printf("(console did not answer: %v)\n", err)
	} else if !named {
		fmt.Printf("(no %s greeting — this LK predates the bridge magic; the console still works)\n", mviiDebugDeviceMagic)
	}

	if script != "" {
		return runMVIIDebugScript(port, script)
	}
	return runMVIIDebugInteractive(port)
}

// Boot modes accepted by -flag, and by the console's own `flag` command.
//
// Their whole purpose is to make "get the board into mode X" one command from
// the host instead of a power-cycle plus a BROM menu plus a wait. They are set
// through the live console, so the board has to be *in* the console already --
// which it is for the entire length of a debugging session, which is when
// switching modes is worth doing.
const (
	bootFlagDebug = "debug" // next boot serves the live console
	bootFlagBoot  = "boot"  // next boot runs the kernel
	bootFlagBROM  = "brom"  // reset now into BROM USB download mode
)

func validBootFlag(mode string) bool {
	switch mode {
	case bootFlagDebug, bootFlagBoot, bootFlagBROM:
		return true
	}
	return false
}

// waitForMVIIDebugPort retries the open until the console shows up, which after
// a reset it will not do for several seconds: the board has to run the BROM, the
// stock preloader, our LK, and an eMMC init before the gadget enumerates.
func waitForMVIIDebugPort(timeout time.Duration) (mviiDebugPort, error) {
	deadline := time.Now().Add(timeout)
	for {
		port, err := openMVIIDebugPort()
		if err == nil {
			return port, nil
		}
		if time.Now().After(deadline) {
			return nil, err
		}
		time.Sleep(500 * time.Millisecond)
	}
}

// setMVIIBootFlagOverConsole sets what the next boot does, then makes it happen.
//
// `debug` and `boot` are the eMMC one-shot flag, so they need a reset to take
// effect and this issues it. `brom` is the BROM's own USBDL latch, which the
// device consumes in the reset it performs itself -- so no `reboot` is sent
// after it, and none should be.
//
// With reattach set (-flag debug -dbg, or -flag debug -dbg-run "..."), this
// waits for the console to come back and hands over to it, which turns the
// whole "re-arm, power-cycle, re-attach, retype" loop into one invocation.
//
// Returns handled=false, and no error, when there is no console to talk to.
// That is not a failure: a board in BROM has no console by definition, and the
// caller's answer to that is to flash it and let the payload arm the flag on
// its way out (applyBootFlagOverMTKFeed).
func setMVIIBootFlagOverConsole(mode string, reattach bool, script string) (bool, error) {
	if !validBootFlag(mode) {
		return false, fmt.Errorf("-flag %q: expected %s, %s or %s", mode, bootFlagDebug, bootFlagBoot, bootFlagBROM)
	}

	port, err := openMVIIDebugPort()
	if err != nil {
		if errors.Is(err, errMVIIDebugNotFound) {
			return false, nil
		}
		return false, fmt.Errorf("open the live console for -flag: %w", err)
	}

	fmt.Printf("MVII live debug console attached (0x0e8d:0x4d56); setting next boot = %s\n", mode)
	if _, err := syncMVIIDebug(port, 5*time.Second, false); err != nil {
		port.Close()
		return true, err
	}

	if err := sendMVIIDebugLine(port, "flag "+mode); err != nil {
		port.Close()
		return true, fmt.Errorf("send flag %s: %w", mode, err)
	}
	// `flag brom` resets inside the command, so the link dropping here IS the
	// success case for it and an error means only that we lost the transcript.
	out, drainErr := drainMVIIDebug(port, 30*time.Second, true)
	if drainErr != nil && mode != bootFlagBROM {
		port.Close()
		return true, fmt.Errorf("after flag %s: %w", mode, drainErr)
	}
	if strings.Contains(out, "FAILED") {
		port.Close()
		return true, fmt.Errorf("the device refused to arm the flag; see the transcript above")
	}
	// Refuse to act on an answer that does not name the mode we asked for.
	//
	// The LK that shipped before this feature has a `flag` command that takes no
	// argument and unconditionally arms DEBUG. Talking to one of those, `-flag
	// boot` would arm the console and `-flag brom` would arm the console and then
	// be reported here as a BROM reset that never happened -- both the opposite of
	// what was asked, and silent. The new command always echoes the mode, so its
	// absence is a clean way to say "that board needs the newer lk.bin".
	if !strings.Contains(out, "next boot =") && !strings.Contains(out, "USBDL") {
		port.Close()
		return true, fmt.Errorf("the board answered `flag` without naming a mode, so it is running an LK from "+
			"before -flag existed and just armed the debug console regardless of the %q you asked for; "+
			"flash the current boot/lk.bin first", mode)
	}

	if mode != bootFlagBROM {
		if err := sendMVIIDebugLine(port, "reboot"); err != nil {
			port.Close()
			return true, fmt.Errorf("send reboot: %w", err)
		}
		_, _ = drainMVIIDebug(port, 10*time.Second, true)
	}
	port.Close()

	return true, reportBootFlagReset(mode, reattach, script)
}

// reportBootFlagReset says what the reset that just happened will produce, and
// -- for the one mode that comes back to a console -- optionally waits for it.
//
// Shared by both halves of -flag, because the outcome is the same whether the
// mode was armed over the live console or over the BROM payload: the board is
// resetting, and only `debug` brings back something to talk to.
func reportBootFlagReset(mode string, reattach bool, script string) error {
	switch mode {
	case bootFlagBROM:
		fmt.Println("\nboard reset into BROM download mode; ./flash without -dbg now talks to it")
		return nil
	case bootFlagBoot:
		fmt.Println("\nboard reset into a normal boot")
		return nil
	}

	fmt.Println("\nboard reset; it will come back up in the debug console")
	if !reattach {
		fmt.Println("attach with: ./flash -target=arm -dbg")
		return nil
	}

	fmt.Print("waiting for the console to re-enumerate")
	next, err := waitForMVIIDebugPort(90 * time.Second)
	if err != nil {
		fmt.Println()
		return fmt.Errorf("the console did not come back within 90s: %w", err)
	}
	defer next.Close()
	fmt.Println(" — attached.")

	if _, err := syncMVIIDebug(next, 15*time.Second, true); err != nil {
		return err
	}
	if script != "" {
		return runMVIIDebugScript(next, script)
	}
	return runMVIIDebugInteractive(next)
}

// runMVIIDebugScript is the anti-ping-pong path: one invocation, one paste.
func runMVIIDebugScript(port mviiDebugPort, script string) error {
	commands := make([]string, 0, 8)
	for _, raw := range strings.Split(script, ";") {
		cmd := strings.TrimSpace(raw)
		if cmd != "" {
			commands = append(commands, cmd)
		}
	}
	if len(commands) == 0 {
		return errors.New("-dbg-run was empty; pass commands separated by ';'")
	}

	budget := envDuration("MVII_DBG_BUDGET", mviiDebugCommandBudget)
	fmt.Printf("running %d command(s)\n", len(commands))
	for i, cmd := range commands {
		fmt.Printf("\n===== [%d/%d] %s =====\n", i+1, len(commands), cmd)
		if err := sendMVIIDebugLine(port, cmd); err != nil {
			return fmt.Errorf("send %q: %w", cmd, err)
		}
		// `boot`, `reboot` and `flag brom` take the device away on purpose, so a
		// dropped link after them is the expected outcome and not a failure to
		// report. `flag debug` and `flag boot` only arm a sector and must not be
		// on this list -- a detach after either of those is a real fault.
		out, err := drainMVIIDebug(port, budget, true)
		if err != nil {
			verb := strings.Fields(cmd)
			detaches := len(verb) > 0 && (verb[0] == "boot" || verb[0] == "reboot" ||
				(verb[0] == "flag" && len(verb) > 1 && verb[1] == bootFlagBROM))
			if detaches {
				fmt.Printf("\n(device detached after %s, as expected)\n", cmd)
				return nil
			}
			return fmt.Errorf("after %q: %w", cmd, err)
		}
		_ = out
	}
	fmt.Println("\nall commands complete; the board is still in the console (send `boot` to continue booting)")
	return nil
}

// runMVIIDebugInteractive is a full-duplex terminal on the console: device
// output is printed the instant it arrives, in its own goroutine, while stdin
// stays free the whole time.
//
// It used to take turns -- send a line, then block until the reply looked
// finished, then read stdin again -- and taking turns is exactly wrong for this
// device. `kpdmon` streams a line per button press for as long as the operator
// keeps pressing, and `ledscan` steps through channels on its own clock. Under
// turn-taking, watching either of those meant the keyboard was dead until the
// command decided it was done, so there was no way to interrupt a scan, no way
// to queue the next probe while a slow one ran, and any output the device
// volunteered between commands sat unread in the FIFO until something was typed.
func runMVIIDebugInteractive(port mviiDebugPort) error {
	fmt.Println("Live. Device output streams as it happens; type at any time.")
	fmt.Println("`help` for commands, `boot` to let the board finish booting, Ctrl-D to detach.")

	// Writer: stdin in its own goroutine so the select below can also notice the
	// device going away, which a bare blocking ReadString never would.
	//
	// Started once and shared by every reconnection below. It has to outlive the
	// port: a line typed while the cable is out is still a line the operator
	// meant, and tearing this down and rebuilding it per session would drop
	// whatever was mid-read at the moment the link went.
	lines := make(chan string)
	stdinDone := make(chan struct{})
	go func() {
		defer close(stdinDone)
		reader := bufio.NewReader(os.Stdin)
		for {
			line, err := reader.ReadString('\n')
			if line != "" {
				lines <- line
			}
			if err != nil {
				return
			}
		}
	}()

	// One iteration per link. A dropped link is a reconnect, not an exit.
	//
	// The device end stopped treating a lost link as the end of the session --
	// the bridge stays up and goes back to waiting for a host (see the outer
	// loop in mvii_debug_console_run) -- and this is the half that makes that
	// worth having. Before it, unplugging the board for ten seconds, restarting
	// this tool, or letting another `flash` invocation touch the bus cost the
	// operator the whole terminal, and the way back was a power cycle.
	//
	// Only a deliberate detach (Ctrl-D, `quit`, `exit`) and a reattach that
	// times out leave this loop. `boot` does not: the board goes away, we wait,
	// and if the operator double-taps MENU on the charge screen it comes back to
	// the same terminal.
	for {
		// Reader: everything the device says, straight to stdout. One per link,
		// because it captures the port it was started on.
		readerDone := make(chan error, 1)
		go func(p mviiDebugPort) {
			buf := make([]byte, 4096)
			for {
				n, err := p.ReadSome(buf, 200*time.Millisecond)
				if n > 0 {
					os.Stdout.Write(buf[:n])
				}
				if err != nil {
					readerDone <- err
					return
				}
			}
		}(port)

		var lost error
	session:
		for {
			select {
			case err := <-readerDone:
				lost = err
				break session
			case <-stdinDone:
				fmt.Println("\ndetaching; the board is still in the console")
				return nil
			case line := <-lines:
				cmd := strings.TrimSpace(line)
				if cmd == "quit" || cmd == "exit" {
					fmt.Println("detaching; the board is still in the console")
					return nil
				}
				if sendErr := sendMVIIDebugLine(port, cmd); sendErr != nil {
					lost = fmt.Errorf("send %q: %w", cmd, sendErr)
					break session
				}
			}
		}

		fmt.Printf("\n(link dropped: %v)\n", lost)
		fmt.Print("the bridge is still up on the board; waiting for it to come back")
		// Closing the old handle before reopening matters on macOS: libusb will
		// hand out a second handle to the same device and then neither can claim
		// the interface. Close is idempotent, so the caller's deferred close is
		// still safe.
		port.Close()
		next, err := waitForMVIIDebugPort(mviiDebugReattachWait)
		if err != nil {
			fmt.Println()
			return fmt.Errorf("the console did not come back within %s (double-tap MENU on the charge screen to "+
				"reopen it, or power-cycle holding a button): %w", mviiDebugReattachWait, err)
		}
		fmt.Println(" — reattached.")
		port = next

		named, err := syncMVIIDebug(port, 15*time.Second, true)
		if err != nil {
			return err
		}
		if !named {
			fmt.Printf("(no %s greeting after the reconnect — this may be a freshly booted board rather than the "+
				"session you left)\n", mviiDebugDeviceMagic)
		}
	}
}
