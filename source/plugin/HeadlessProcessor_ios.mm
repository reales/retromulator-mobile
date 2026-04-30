#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include "juce_core/system/juce_TargetPlatform.h"
#include "HeadlessProcessor.h"
#include "dsp56kEmu/memorybuffer.h"

namespace retromulator
{
    std::string HeadlessProcessor::getIOSSharedDataFolder()
    {
        NSString* groupId = @"group.com.discodsp.retromulator";
        NSURL* containerURL = [[NSFileManager defaultManager]
            containerURLForSecurityApplicationGroupIdentifier: groupId];

        if (containerURL)
            return std::string([[containerURL path] UTF8String]) + "/discoDSP/Retromulator/";

        // Fallback when App Groups are not provisioned yet — use Documents.
        // The app still works; files won't be shared between the standalone and AUv3.
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* docs = [paths firstObject];
        return std::string([docs UTF8String]) + "/discoDSP/Retromulator/";
    }

    void HeadlessProcessor::setIOSPreferredBufferSize(int samples)
    {
        NSError* error = nil;
        AVAudioSession* session = [AVAudioSession sharedInstance];
        double duration = static_cast<double>(samples) / session.sampleRate;
        [session setPreferredIOBufferDuration:duration error:&error];
        if (error)
            fprintf(stderr, "[iOS] Failed to set preferred buffer duration: %s\n",
                [[error localizedDescription] UTF8String]);
        else
        {
            int actualSamples = (int)round(session.IOBufferDuration * session.sampleRate);
            fprintf(stderr, "[iOS] Audio latency: requested=%d samples, actual=%d samples (%.1f ms at %.0f Hz)\n",
                samples, actualSamples, session.IOBufferDuration * 1000.0, session.sampleRate);
        }
    }

    void HeadlessProcessor::linkDocumentsToSharedFolder()
    {
        // iTunes/Finder File Sharing exposes the app's Documents directory.
        // Since all data lives in the App Group container, place a symlink
        // "Retromulator" inside Documents pointing to the shared folder so
        // users can browse ROMs and presets via iTunes/Finder.
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* docs = [paths firstObject];
        if (!docs)
            return;

        NSString* groupId = @"group.com.discodsp.retromulator";
        NSURL* containerURL = [[NSFileManager defaultManager]
            containerURLForSecurityApplicationGroupIdentifier: groupId];
        if (!containerURL)
            return;

        NSString* sharedPath = [[containerURL path] stringByAppendingPathComponent:@"discoDSP/Retromulator"];
        NSString* linkPath   = [docs stringByAppendingPathComponent:@"Retromulator"];

        NSFileManager* fm = [NSFileManager defaultManager];

        // Ensure the shared folder exists
        [fm createDirectoryAtPath:sharedPath withIntermediateDirectories:YES attributes:nil error:nil];

        // If the link already exists and points to the right place, nothing to do
        NSDictionary* attrs = [fm attributesOfItemAtPath:linkPath error:nil];
        if (attrs && [attrs[NSFileType] isEqualToString:NSFileTypeSymbolicLink])
        {
            NSString* dest = [fm destinationOfSymbolicLinkAtPath:linkPath error:nil];
            if ([dest isEqualToString:sharedPath])
                return;
            // Wrong target — remove and recreate
            [fm removeItemAtPath:linkPath error:nil];
        }
        else if (attrs)
        {
            // A regular file/folder exists at the link path — don't overwrite
            return;
        }

        NSError* error = nil;
        if (![fm createSymbolicLinkAtPath:linkPath withDestinationPath:sharedPath error:&error])
            fprintf(stderr, "[iOS] Failed to create Documents symlink: %s\n",
                    [[error localizedDescription] UTF8String]);
    }

    void HeadlessProcessor::initIOSTempPath()
    {
        // iOS sandboxing blocks shm_open. The DSP56300 MemoryBuffer falls back to
        // temp-file-backed mmap, but needs a writable directory. Use the system
        // temporary directory which is always writable inside the app sandbox.
        NSString* tmp = NSTemporaryDirectory();
        if (tmp)
            dsp56k::MemoryBuffer::setTempPath(std::string([tmp UTF8String]));
    }

}
