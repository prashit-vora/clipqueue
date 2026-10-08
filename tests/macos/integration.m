/* Development-only AppKit test host. The distributed daemon does not link AppKit. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
static NSTask *daemon;
static NSString *executable;
static NSWindow *window;
static NSTextView *source, *sink;
static void pump(double seconds) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    do {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]))
            [NSApp sendEvent:event];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
    } while ([end timeIntervalSinceNow] > 0);
}
static void check(BOOL ok, const char *label) {
    if (ok)
        return;
    fprintf(stderr, "FAIL: %s\n", label);
    if (daemon.running)
        [daemon terminate];
    exit(1);
}
static NSString *ctl(NSString *cmd) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:executable];
    task.arguments = @[ cmd ];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    NSError *error = nil;
    check([task launchAndReturnError:&error], "launch control client");
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    NSString *reply = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    check(task.terminationStatus == 0, reply.UTF8String);
    return reply;
}
static void count(unsigned expected) {
    NSString *reply = ctl(@"status"), *part = [NSString stringWithFormat:@", %u queued", expected];
    check([reply containsString:part], reply.UTF8String);
}
static void focus(NSTextView *view) {
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [window makeFirstResponder:view];
    pump(0.1);
    check(window.keyWindow && window.firstResponder == view, "focus native text view");
}
static void keys(CGKeyCode code, unsigned times, CGEventFlags flags) {
    for (unsigned i = 0; i < times; i++) {
        CGEventRef down = CGEventCreateKeyboardEvent(NULL, code, true),
                   up = CGEventCreateKeyboardEvent(NULL, code, false);
        CGEventSetFlags(down, flags);
        CGEventSetFlags(up, flags);
        CGEventPost(kCGSessionEventTap, down);
        CGEventPost(kCGSessionEventTap, up);
        CFRelease(down);
        CFRelease(up);
    }
}
static void copy(NSString *text) {
    focus(source);
    source.string = text;
    source.selectedRange = NSMakeRange(0, text.length);
    keys(8, 1, kCGEventFlagMaskCommand);
    pump(0.25);
}
static void expect(NSString *wanted, const char *label) {
    if (![sink.string isEqualToString:wanted])
        fprintf(stderr, "Expected [%s], got [%s]\n", wanted.UTF8String, sink.string.UTF8String);
    check([sink.string isEqualToString:wanted], label);
    printf("PASS: %s\n", label);
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        check(argc == 2, "daemon executable argument");
        executable = [NSString stringWithUTF8String:argv[1]];
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 600, 300)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.title = @"ClipQueue native integration test";
        source = [[NSTextView alloc] initWithFrame:NSMakeRect(10, 160, 580, 120)];
        sink = [[NSTextView alloc] initWithFrame:NSMakeRect(10, 10, 580, 120)];
        source.richText = NO;
        sink.richText = NO;
        [window.contentView addSubview:source];
        [window.contentView addSubview:sink];
        focus(sink);
        daemon = [NSTask new];
        daemon.executableURL = [NSURL fileURLWithPath:executable];
        daemon.arguments = @[ @"--daemon" ];
        NSError *error = nil;
        check([daemon launchAndReturnError:&error], "start daemon");
        pump(0.6);
        check(daemon.running, "daemon running (Accessibility permission required)");
        keys(12, 1, kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate);
        pump(0.2);
        check([ctl(@"status") containsString:@": on,"], "toggle via Command+Option+Q");
        copy(@"A");
        copy(@"A");
        copy(@"B");
        copy(@"C");
        count(3);
        focus(sink);
        keys(9, 3, kCGEventFlagMaskCommand);
        pump(1.2);
        expect(@"ABC", "three rapid Command+V presses paste three FIFO entries");
        count(0);
        ctl(@"undo");
        count(1);
        keys(9, 1, kCGEventFlagMaskCommand);
        pump(0.35);
        expect(@"ABCC", "undo restores last paste");
        keys(9, 1, kCGEventFlagMaskCommand);
        pump(0.2);
        expect(@"ABCCC", "empty queue passes through");
        keys(12, 1, kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate);
        pump(0.2);
        copy(@"paused");
        count(0);
        focus(sink);
        keys(9, 1, kCGEventFlagMaskCommand);
        pump(0.2);
        expect(@"ABCCCpaused", "paused paste passes through");
        keys(12, 1, kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate);
        pump(0.2);
        copy(@"clear me");
        count(1);
        focus(sink);
        keys(51, 1, kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate);
        pump(0.2);
        count(0);
        copy(@"snowman ☃");
        focus(sink);
        keys(9, 1, kCGEventFlagMaskCommand);
        pump(0.35);
        expect(@"ABCCCpausedsnowman ☃", "Unicode keyboard paste");
        ctl(@"stop");
        [daemon waitUntilExit];
        check(daemon.terminationStatus == 0, "graceful exit");
        puts("PASS: macOS real-window keyboard integration");
        return 0;
    }
}
