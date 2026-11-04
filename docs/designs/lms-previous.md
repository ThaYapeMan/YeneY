# LMS Previous navigation

## Investigation

The bridge receives SlimProto `strm q` followed by `strm s`, not a Previous
opcode. LMS status supplies the selected playlist index and time. This was
checked against the private loopback LMS 9.1.1 distribution and its protocol
transcript, without changing or copying server code into the bridge.

In that server, `playlist index -1` and `playlist jump -1` select the prior
index regardless of elapsed time. `button jump_rew` selects the prior index
below five seconds and restarts the current track otherwise. The older `rew`
button also considers the interval between button presses. These are server
decisions; the bridge must follow the resulting status rather than implement
its own button threshold. A restart and an explicit seek to zero have the same
wire representation.

`QueueRoom::Impl::command` previously ignored a stop whenever a pending lazy
target matched the current status index. That suppression lasted until lazy
publication, rather than consuming one paired selection echo. A subsequent
controller stop could consequently bypass navigation capture. A start outside
that capture uses the natural-successor index rule. `reconcileWindow` also
labelled *all* surgical edits `reason=playlist-change`, even when its final
diagnostic correctly identified `kind=previous`. The field's edit reason alone
therefore does not establish that the final navigation classifier ran.

Two teardown paths independently explain how a backward move can interrupt
audio: navigation cancelled an unresolved target's lazy route instead of
binding the supplied producer, and backward reconciliation retired an
interrupted upcoming parent while that parent was still audible. In the early
Next/Back regression the old implementation removes the current parent before
the final target seek. The fake speaker exposes an empty current URI at that
point; a device that advances on removal can briefly play its following row.
Restarting an incomplete current parent had the same removal-before-seek
problem. The exact field device's 2057 ms duration was not reproduced; it is
not a fixed delay in this navigation code. Lazy readers log their actual wait
until cancellation, disconnect, publication, or their existing deadline.

## Implementation

- Capture controller stops uniformly and confirm a lazy selection echo once,
  using status at its paired start. Later stops retain navigation authority.
- Reject a window status read superseded by queued controller work.
- Bind a controller-supplied producer into a target lazy route with a waiting
  reader. That reader keeps its URI and does not issue another selection.
- Keep an interrupted, still-audible upcoming parent until the target has been
  selected; refresh its declaration on the next worker pass. Retire storage
  after the corresponding speaker edits.
- For a current-row restart requiring a new declaration, insert and seek the
  replacement before removing the old row. Browse, exact URI-vector checks,
  and UpdateID confirmation surround each mutation. There is one target seek
  and the existing Play, without another LMS command.
- Label navigation edits `reason=lms-navigation`. Adjacent backward moves and
  same-index starts at zero emit `kind=previous`; nonzero seeks retain
  `kind=seek`. Existing history limits and out-of-range rebuild rules remain.

## Decisions

1. Keep the fix in the bridge; no core or submodule pin change is needed.
2. Treat LMS status as authoritative for Previous versus restart. Do not infer
   a button press from elapsed time in the bridge. Since zero seeks and Back
   restarts are indistinguishable on the wire, diagnose both as `previous`.
3. Preserve two history parents and use the existing fenced window edit path.
   A restart's declaration replacement needs insert/seek/remove ordering to
   avoid removing the audible row first.
4. A lazy activation queued during a controller navigation yields to that
   controller's producer. It must not echo `playlist index` back to LMS.
5. Preserve the existing lazy HTTP deadline, routes, audio declarations,
   transport commands, and direct-binary lifecycle. No runtime service or
   owner-LMS access is required.
6. Test early Back with a waiting PCM GET, late Back through LMS's actual
   `jump_rew` button, and Back into retained history. Check intermediate edit
   notifications as well as the final row; final-row checks alone missed the
   teardown gap. Keep the existing natural advance, jump, seek, ownership,
   byte-identity, and speaker-originated lazy-selection guards.
7. Leave pre-existing untracked field fixtures untouched. Use the explicitly
   supplied read-only LampaStream consumer checkout for required validation.
8. Check the podcast bookmark seed's actual 100-second PCM frame before
   reporting that position. Full validation exposed a stale earlier-episode
   log match, which let the fake speaker jump ahead of production and generated
   underruns instead of a bookmark. Waiting for the seed's complete item-end
   is inappropriate because MP3 end resolution can wait for audible progress.
   A bounded six-byte Range read confirms the needed audio using the existing
   15-second readiness budget; production timing remains unchanged.
