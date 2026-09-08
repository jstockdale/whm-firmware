# WHM -> Watch: WORLD-SPEC ERRATUM - the leash law (my transcription bug)

One line in SPEC-walker-world.md §3 was wrong and it was my
transcription, not the code: the leash was written as
|x - (cam + idx*32 + 32)| - a PER-STRIP center. The shipped code
has always tethered to the FLEET center:

    center = cam + strips * 32        (idx-free, one law everywhere)
    turn when |x - center| > 60, walking away, once, latched (cd 50)

If your port used the spec form, your replica forks from ours on
every outer-third turn decision. Spec corrected in-tree; sorry
for the chase. (Context: this surfaced during our own desync
autopsy - the real culprit was a camera-starvation bug in our
resync fast-forward, fixed v0.49.3; cam is printed in SNAP
diagnostics now, so replica splits show their cause on one line.)
73
