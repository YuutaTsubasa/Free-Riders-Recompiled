#include "ios_document_picker.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface SFRDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL done;
@property(nonatomic, strong) NSURL* picked;
@end

@implementation SFRDocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.picked = urls.firstObject;
    self.done = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    self.done = YES;
}
@end

namespace sfr::launcher {
namespace {
// The picked files whose access is kept until the app ends.
NSMutableArray<NSURL*>* opened_files() {
    static NSMutableArray<NSURL*>* files = [NSMutableArray array];
    return files;
}

UIViewController* top_view_controller() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow* window in static_cast<UIWindowScene*>(scene).windows) {
            if (!window.isKeyWindow) continue;
            UIViewController* controller = window.rootViewController;
            while (controller.presentedViewController) controller = controller.presentedViewController;
            return controller;
        }
    }
    return nil;
}
}  // namespace

std::optional<std::string> ios_pick_document(bool avatar_model) {
    @autoreleasepool {
        UIViewController* parent = top_view_controller();
        if (!parent) return std::nullopt;
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        if (avatar_model) {
            for (NSString* extension in @[@"vrm", @"glb"])
                if (UTType* type = [UTType typeWithFilenameExtension:extension]) [types addObject:type];
        }
        if (types.count == 0) [types addObject:UTTypeItem];
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
        SFRDocumentPickerDelegate* delegate = [SFRDocumentPickerDelegate new];
        picker.delegate = delegate;
        [parent presentViewController:picker animated:YES completion:nil];
        // The launcher waits here, as it does for a desktop dialog; UIKit
        // keeps running through the main run loop.
        while (!delegate.done)
            @autoreleasepool {
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
        NSURL* picked = delegate.picked;
        if (!picked || !picked.fileURL) return std::nullopt;
        if ([picked startAccessingSecurityScopedResource]) [opened_files() addObject:picked];
        return std::string(picked.path.fileSystemRepresentation);
    }
}

}  // namespace sfr::launcher
