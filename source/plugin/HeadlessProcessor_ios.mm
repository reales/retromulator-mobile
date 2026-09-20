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

    // JUCE has no AAC encoder: CoreAudioFormat is read-only and its createWriterFor is a
    // stub. The render writes WAV as usual and this converts that file, so the emulation
    // path stays the one that is already known good.
    bool HeadlessProcessor::encodeWavToAac(const std::string& wavPath, const std::string& aacPath,
                                           const int bitRate)
    {
        @autoreleasepool
        {
            NSURL* src = [NSURL fileURLWithPath:[NSString stringWithUTF8String:wavPath.c_str()]];
            NSURL* dst = [NSURL fileURLWithPath:[NSString stringWithUTF8String:aacPath.c_str()]];
            [[NSFileManager defaultManager] removeItemAtURL:dst error:nil];

            NSError* error = nil;

            // AVAudioFile writes m4a that players reject: it leaves the container
            // without a usable index. AVAssetReader feeding AVAssetWriter is the
            // supported route, and it also allows the bit rate to be named.
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:src options:nil];
            AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeAudio] firstObject];
            if (!track)
            {
                fprintf(stderr, "[88emu render] AAC: source has no audio track\n");
                return false;
            }

            AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
            if (!reader)
            {
                fprintf(stderr, "[88emu render] AAC: reader failed: %s\n",
                        [[error localizedDescription] UTF8String]);
                return false;
            }

            // Decode to packed 16-bit PCM, which is what the AAC encoder wants to be fed.
            NSDictionary* readerSettings = @{
                AVFormatIDKey              : @(kAudioFormatLinearPCM),
                AVLinearPCMBitDepthKey     : @16,
                AVLinearPCMIsBigEndianKey  : @NO,
                AVLinearPCMIsFloatKey      : @NO,
                AVLinearPCMIsNonInterleaved: @NO,
            };
            AVAssetReaderTrackOutput* readerOutput =
                [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                           outputSettings:readerSettings];
            if (![reader canAddOutput:readerOutput])
                return false;
            [reader addOutput:readerOutput];

            AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:dst
                                                             fileType:AVFileTypeAppleM4A
                                                                error:&error];
            if (!writer)
            {
                fprintf(stderr, "[88emu render] AAC: writer failed: %s\n",
                        [[error localizedDescription] UTF8String]);
                return false;
            }
            // Puts the moov atom at the front, so the file is playable rather than
            // depending on an index appended at the very end.
            writer.shouldOptimizeForNetworkUse = YES;

            AudioChannelLayout layout = {};
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            NSDictionary* writerSettings = @{
                AVFormatIDKey         : @(kAudioFormatMPEG4AAC),
                AVSampleRateKey       : @(48000),
                AVNumberOfChannelsKey : @(2),
                AVEncoderBitRateKey   : @(bitRate),
                AVChannelLayoutKey    : [NSData dataWithBytes:&layout length:sizeof(layout)],
            };
            AVAssetWriterInput* writerInput =
                [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                   outputSettings:writerSettings];
            writerInput.expectsMediaDataInRealTime = NO;
            if (![writer canAddInput:writerInput])
                return false;
            [writer addInput:writerInput];

            if (![writer startWriting] || ![reader startReading])
            {
                fprintf(stderr, "[88emu render] AAC: cannot start: %s\n",
                        [[writer.error localizedDescription] UTF8String]);
                return false;
            }
            // CMTime is a plain struct, so zero is built here. kCMTimeZero and
            // CMTimeMake are both CoreMedia symbols, which this target does not link.
            CMTime zero = {};
            zero.timescale = 1;
            zero.flags = kCMTimeFlags_Valid;
            [writer startSessionAtSourceTime:zero];

            // requestMediaDataWhenReady is asynchronous, so the render thread waits on
            // this rather than returning before the file exists.
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            dispatch_queue_t queue = dispatch_queue_create("com.discodsp.retromulator.aac", NULL);

            [writerInput requestMediaDataWhenReadyOnQueue:queue usingBlock:^
            {
                while (writerInput.readyForMoreMediaData)
                {
                    CMSampleBufferRef sample = [readerOutput copyNextSampleBuffer];
                    if (sample)
                    {
                        const BOOL ok = [writerInput appendSampleBuffer:sample];
                        CFRelease(sample);
                        if (ok)
                            continue;
                    }
                    [writerInput markAsFinished];
                    dispatch_semaphore_signal(done);
                    return;
                }
            }];

            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

            __block bool finished = false;
            dispatch_semaphore_t wrote = dispatch_semaphore_create(0);
            [writer finishWritingWithCompletionHandler:^
            {
                finished = writer.status == AVAssetWriterStatusCompleted;
                dispatch_semaphore_signal(wrote);
            }];
            dispatch_semaphore_wait(wrote, DISPATCH_TIME_FOREVER);

            if (!finished)
                fprintf(stderr, "[88emu render] AAC: finish failed: %s\n",
                        [[writer.error localizedDescription] UTF8String]);
            return finished;
        }
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


// ── Documents handed over by Files, "Open in" and share sheets ───────────────
// JUCE's delegates implement none of the open-URL callbacks, so they are grafted
// on at load time rather than patching the module. With the UIScene lifecycle a
// running app gets scene:openURLContexts:, and a cold launch gets the contexts in
// the connection options of scene:willConnectToSession:options:.

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

namespace
{
    retromulator::HeadlessProcessor* g_documentTarget = nullptr;
    NSMutableArray<NSString*>* g_pendingDocuments = nil;   // message thread only

    // A multi-select share arrives one file at a time, so arrivals are gathered until
    // they stop and then opened as one batch: several modules make a playlist.
    int g_flushGeneration = 0;

    void flushDocuments()
    {
        if(g_documentTarget == nullptr || g_pendingDocuments == nil || g_pendingDocuments.count == 0)
            return;

        NSArray<NSString*>* pending = [g_pendingDocuments copy];
        [g_pendingDocuments removeAllObjects];

        std::vector<juce::URL> urls;
        for(NSString* path in pending)
            urls.push_back(juce::URL(juce::File(juce::String::fromUTF8([path UTF8String]))));

        g_documentTarget->openDocuments(urls);

        // what is kept was copied in, so the temp files are done with
        for(NSString* path in pending)
            [[NSFileManager defaultManager] removeItemAtPath: path error: nil];
        [pending release];
    }

    void deliverDocument(NSString* path)
    {
        if(g_pendingDocuments == nil)
            g_pendingDocuments = [[NSMutableArray alloc] init];
        [g_pendingDocuments addObject: path];

        const int generation = ++g_flushGeneration;
        juce::Timer::callAfterDelay(400, [generation]
        {
            if(generation == g_flushGeneration)
                flushDocuments();
        });
    }

    // The file is opened in place, outside the sandbox: it is copied to a temp file
    // under scoped access, off the main thread since an iCloud item may need a download.
    void handleIncomingURL(NSURL* url)
    {
        if(url == nil || ![url isFileURL])
            return;

        NSURL* copy = [url copy];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^
        {
            const BOOL scoped = [copy startAccessingSecurityScopedResource];

            NSString* dest = [NSTemporaryDirectory() stringByAppendingPathComponent: [copy lastPathComponent]];
            __block BOOL ok = NO;
            NSFileCoordinator* coord = [[NSFileCoordinator alloc] initWithFilePresenter: nil];
            [coord coordinateReadingItemAtURL: copy options: 0 error: nil byAccessor: ^(NSURL* readable)
            {
                NSFileManager* fm = [NSFileManager defaultManager];
                [fm removeItemAtPath: dest error: nil];
                ok = [fm copyItemAtURL: readable toURL: [NSURL fileURLWithPath: dest] error: nil];
            }];
            [coord release];

            if(scoped)
                [copy stopAccessingSecurityScopedResource];
            [copy release];

            if(!ok)
                return;

            NSString* path = [dest copy];
            juce::MessageManager::callAsync([path]
            {
                deliverDocument(path);
                [path release];
            });
        });
    }

    BOOL retroApplicationOpenURL(id, SEL, UIApplication*, NSURL* url, NSDictionary*)
    {
        handleIncomingURL(url);
        return YES;
    }

    void retroSceneOpenURLContexts(id, SEL, UIScene*, NSSet<UIOpenURLContext*>* contexts)
    {
        for(UIOpenURLContext* context in contexts)
            handleIncomingURL(context.URL);
    }

    IMP g_originalWillConnect = nullptr;

    void retroSceneWillConnect(id self, SEL cmd, UIScene* scene, UISceneSession* session,
                               UISceneConnectionOptions* options)
    {
        if(g_originalWillConnect != nullptr)
            ((void (*)(id, SEL, UIScene*, UISceneSession*, UISceneConnectionOptions*)) g_originalWillConnect)
                (self, cmd, scene, session, options);

        for(UIOpenURLContext* context in options.URLContexts)
            handleIncomingURL(context.URL);
    }
}

namespace retromulator
{
    void HeadlessProcessor::setIOSDocumentTarget(HeadlessProcessor* target)
    {
        g_documentTarget = target;
        if(target == nullptr || g_pendingDocuments == nil)
            return;

        // The core switch needs a finished constructor, so the queue drains afterwards.
        juce::MessageManager::callAsync([] { flushDocuments(); });
    }
}

@interface RetromulatorOpenURLInstaller : NSObject
@end

@implementation RetromulatorOpenURLInstaller

// The JUCE delegate classes are registered by the runtime before any +load runs.
+ (void) load
{
    if(Class c = objc_getClass("JuceAppStartupDelegate"))
        class_addMethod(c, @selector(application:openURL:options:),
                        (IMP) retroApplicationOpenURL, "B@:@@@");

    if(Class c = objc_getClass("JuceAppSceneDelegate"))
    {
        class_addMethod(c, @selector(scene:openURLContexts:),
                        (IMP) retroSceneOpenURLContexts, "v@:@@");

        if(Method m = class_getInstanceMethod(c, @selector(scene:willConnectToSession:options:)))
            g_originalWillConnect = method_setImplementation(m, (IMP) retroSceneWillConnect);
    }
}

@end
