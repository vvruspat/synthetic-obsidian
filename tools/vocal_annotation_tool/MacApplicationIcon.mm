#include "MacApplicationIcon.h"

#import <AppKit/AppKit.h>

namespace vocal_annotation
{

void refreshMacApplicationIcon()
{
    @autoreleasepool
    {
        auto* iconPath = [[NSBundle mainBundle] pathForResource:@"Icon" ofType:@"icns"];
        if (iconPath == nil)
            return;

        auto* icon = [[[NSImage alloc] initWithContentsOfFile:iconPath] autorelease];
        if (icon != nil)
            [[NSApplication sharedApplication] setApplicationIconImage:icon];
    }
}

} // namespace vocal_annotation
