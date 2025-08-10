set(SOURCES
    audio_codecs/audio_codec.cc
    audio_codecs/no_audio_codec.cc
    main.cc
)

set(INCLUDE_DIRS "." "audio_codecs")

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES "assets/err_reg.p3"
                    INCLUDE_DIRS ${INCLUDE_DIRS}
)