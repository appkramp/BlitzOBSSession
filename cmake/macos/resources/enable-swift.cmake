# OBS 32 builds libobs-metal from Swift sources, and its top-level project()
# does not enable the language — its own builds get it implicitly. Injected
# into the OBS sources' configure by _setup_obs_studio.
if(APPLE)
  enable_language(Swift)
endif()
