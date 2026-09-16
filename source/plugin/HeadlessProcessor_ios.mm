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
