use *;

static CHANNEL_LAYOUT_UNDEFINED: &'static [Channel] = &[ Channel::Invalid ];
static CHANNEL_LAYOUT_DUAL_MONO: &'static [Channel] = &[ Channel::Left, Channel::Right ];
static CHANNEL_LAYOUT_DUAL_MONO_LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::LowFrequency ];
static CHANNEL_LAYOUT_MONO: &'static [Channel] = &[ Channel::Mono ];
static CHANNEL_LAYOUT_MONO_LFE: &'static [Channel] = &[ Channel::Mono, Channel::LowFrequency ];
static CHANNEL_LAYOUT_STEREO: &'static [Channel] = &[ Channel::Left, Channel::Right ];
static CHANNEL_LAYOUT_STEREO_LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::LowFrequency ];
static CHANNEL_LAYOUT_3F: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center ];
static CHANNEL_LAYOUT_3FLFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LowFrequency ];
static CHANNEL_LAYOUT_2F1: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::RearCenter ];
static CHANNEL_LAYOUT_2F1LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::LowFrequency, Channel::RearCenter ];
static CHANNEL_LAYOUT_3F1: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::RearCenter ];
static CHANNEL_LAYOUT_3F1LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LowFrequency, Channel::RearCenter ];
static CHANNEL_LAYOUT_2F2: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::LeftSurround, Channel::RightSurround ];
static CHANNEL_LAYOUT_2F2LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::LowFrequency, Channel::LeftSurround, Channel::RightSurround ];
static CHANNEL_LAYOUT_3F2: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LeftSurround, Channel::RightSurround ];
static CHANNEL_LAYOUT_3F2LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LowFrequency, Channel::LeftSurround, Channel::RightSurround ];
static CHANNEL_LAYOUT_3F3RLFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LowFrequency, Channel::RearCenter, Channel::LeftSurround, Channel::RightSurround ];
static CHANNEL_LAYOUT_3F4LFE: &'static [Channel] = &[ Channel::Left, Channel::Right, Channel::Center, Channel::LowFrequency, Channel::RearLeftSurround, Channel::RearRightSurround, Channel::LeftSurround, Channel::RightSurround ];

pub fn channel_index_to_order(layout: ChannelLayout) -> &'static [Channel]
{
    match layout {
        ChannelLayout::DualMono => CHANNEL_LAYOUT_DUAL_MONO,
        ChannelLayout::DualMonoLfe => CHANNEL_LAYOUT_DUAL_MONO_LFE,
        ChannelLayout::Mono => CHANNEL_LAYOUT_MONO,
        ChannelLayout::MonoLfe => CHANNEL_LAYOUT_MONO_LFE,
        ChannelLayout::Stereo => CHANNEL_LAYOUT_STEREO,
        ChannelLayout::StereoLfe => CHANNEL_LAYOUT_STEREO_LFE,
        ChannelLayout::Layout3F => CHANNEL_LAYOUT_3F,
        ChannelLayout::Layout3FLfe => CHANNEL_LAYOUT_3FLFE,
        ChannelLayout::Layout2F1 => CHANNEL_LAYOUT_2F1,
        ChannelLayout::Layout2F1Lfe => CHANNEL_LAYOUT_2F1LFE,
        ChannelLayout::Layout3F1 => CHANNEL_LAYOUT_3F1,
        ChannelLayout::Layout3F1Lfe => CHANNEL_LAYOUT_3F1LFE,
        ChannelLayout::Layout2F2 => CHANNEL_LAYOUT_2F2,
        ChannelLayout::Layout2F2Lfe => CHANNEL_LAYOUT_2F2LFE,
        ChannelLayout::Layout3F2 => CHANNEL_LAYOUT_3F2,
        ChannelLayout::Layout3F2Lfe => CHANNEL_LAYOUT_3F2LFE,
        ChannelLayout::Layout3F3RLfe => CHANNEL_LAYOUT_3F3RLFE,
        ChannelLayout::Layout3F4Lfe => CHANNEL_LAYOUT_3F4LFE,
        _ => CHANNEL_LAYOUT_UNDEFINED,
    }
}
