// Added to package runtime by build-go-app.sh's -overlay. See GO.md.

package runtime

// Async preemption interrupts a goroutine with SIGURG and resumes it at
// a safe point using the register state in the signal frame. thread_shim.c
// runs signal handlers as plain calls on the interrupted thread's own
// stack, with no real frame, and the Go runtime refuses to handle a signal
// there. This is GODEBUG=asyncpreemptoff=1, which BareMetal apps have no
// environment to set. runtime init functions run before any user code.
// Goroutines are still preempted cooperatively, at function calls.
func init() {
	debug.asyncpreemptoff = 1
}
