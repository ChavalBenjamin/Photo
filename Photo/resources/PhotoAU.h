
#include <TargetConditionals.h>
#if TARGET_OS_IOS == 1 || TARGET_OS_VISION == 1
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif

#define IPLUG_AUVIEWCONTROLLER IPlugAUViewController_vPhoto
#define IPLUG_AUAUDIOUNIT IPlugAUAudioUnit_vPhoto
#import <PhotoAU/IPlugAUViewController.h>
#import <PhotoAU/IPlugAUAudioUnit.h>

//! Project version number for PhotoAU.
FOUNDATION_EXPORT double PhotoAUVersionNumber;

//! Project version string for PhotoAU.
FOUNDATION_EXPORT const unsigned char PhotoAUVersionString[];

@class IPlugAUViewController_vPhoto;
