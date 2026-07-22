# xrPhoton Jolt patches

The files under this directory come from the Jolt Physics v5.6.0 release
archive (tag commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`; downloaded
archive SHA-256
`6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e`).
xrPhoton carries one local exception-safety patch:

- `Jolt/Core/JobSystemThreadPool.cpp`: `StopThreads()` now completes queue and
  `mHeads` cleanup even when no worker thread was started. This lets xrPhoton
  default-construct the pool and call `Init()` inside its creation exception
  boundary without leaking the heads allocation if creation of the first
  worker throws.

Replace the directory wholesale when upgrading Jolt, then re-evaluate and
reapply this patch only if upstream still needs it. Also re-verify
`assertJolt`'s exact update-status expression, message, and `PhysicsSystem.cpp`
file match against the new source; the Debug terminal-update test is the
deliberate tripwire for that string-coupled integration point.
