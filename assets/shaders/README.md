Shader lists shipped in the app (romfs:/data/shaders), one per game: the combiner keys GLideN64 needed on a
Switch across every stage and every fighter of Super Smash Bros., Smash Remix 2.0.1 and +EXTRA 0.6.0
(the stage and fighter matrices, docs/STAGE_MATRIX.md). The three files hold the same list; the name is
GLideN64's (a hash of the ROM's internal name). They are numbers describing colour-combiner settings,
not game data. The app merges them into profile/shaders at start, so those shaders are compiled before
a match rather than during one.
