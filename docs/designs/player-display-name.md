# Player display name

YeneY keeps `<room> (Sonos)` as the true name and the existing MAC as its player
identity. Only the SlimProto player name is configurable. The existing core
SETD response supplies the configured display name on registration and accepts
an LMS rename for the lifetime of that process. Restart reapplies configuration.
No LMS CLI rename is needed for the toggle, and no core modification is needed.

A future LMS plugin will expose “Show (Sonos) suffix” as a checkbox per player,
writing that room's environment setting. The existing global configuration key
`YENEY_SHOW_SONOS_SUFFIX` supplies the shared default. The command-line override,
environment, config and default resolution work independently of systemd.
The plugin must identify players by MAC and use status `true_name` for identity.
