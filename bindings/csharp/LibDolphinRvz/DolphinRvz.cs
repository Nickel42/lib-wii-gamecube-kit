// Managed API for libdolphinrvz: Dolphin's GameCube/Wii disc image
// conversion (RVZ, WIA, GCZ, ISO) as a .NET library.
// SPDX-License-Identifier: GPL-2.0-or-later

using System;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using LibDolphinRvz.Interop;

namespace LibDolphinRvz
{
    public enum DrvzStatus : int
    {
        Ok = 0, InvalidArgument = 1, FileExists = 2, FileOpen = 3, IO = 4, NotADisc = 5, Unsupported = 6,
        Cancelled = 7, OutOfMemory = 8, Internal = 9, AbiMismatch = 10, Conversion = 11
    }

    public enum DrvzPhase : int { Converting = 0, Hashing = 1 }

    /// <summary>Log levels. Treat values you don't know as <see cref="Warning"/>.</summary>
    public enum DrvzLogLevel : int { Info = 0, Warning = 1, Error = 2 }

    public enum DiscContainer : int { Plain = 0, Gcz = 1, Ciso = 2, Wbfs = 3, Tgc = 4, Wia = 5, Rvz = 6, Nfs = 7, Other = 99 }

    public enum DiscPlatform : int { GameCube = 0, Wii = 1, Triforce = 2 }

    public enum DiscRegion : int { NtscJ = 0, NtscU = 1, Pal = 2, Unknown = 3, NtscK = 4, Dev = 5 }

    public enum RvzFormat : int { Iso = 0, Gcz = 1, Wia = 2, Rvz = 3 }

    public enum RvzCompression : int { None = 0, Purge = 1, Bzip2 = 2, Lzma = 3, Lzma2 = 4, Zstd = 5 }

    /// <summary>A libdolphinrvz call failed. <see cref="Status"/> is the C status code.</summary>
    public sealed class DolphinRvzException : Exception
    {
        public DrvzStatus Status { get; }
        public DolphinRvzException(DrvzStatus status, string message) : base(message) { Status = status; }
    }

    /// <summary>One progress report. <see cref="Ratio"/> is output/input size while writing WIA/RVZ, otherwise 0.</summary>
    public readonly record struct DrvzProgress(DrvzPhase Phase, double Fraction, double Ratio);

    public sealed record DiscInfo(
        DiscContainer Container,
        DiscPlatform Platform,
        string GameId,
        string MakerId,
        string InternalName,
        int DiscNumber,
        int Revision,
        DiscRegion Region,
        ulong DataSize,
        bool DataSizeIsExact,
        ulong FileSize,
        uint BlockSize,
        string CompressionMethod,
        int? CompressionLevel,
        bool IsNKit,
        bool IsDatel,
        bool IsScrubbable)
    {
        /// <summary>Dolphin only converts images whose container records the exact disc size (not CISO, WBFS or NFS).</summary>
        public bool CanConvert => DataSizeIsExact;
    }

    public sealed record DiscHashes(string Crc32, string Md5, string Sha1, ulong Size, bool SizeIsExact);

    /// <summary>Conversion settings. The defaults are Dolphin's: RVZ, Zstandard level 5, 128 KiB blocks.</summary>
    public sealed class ConvertSettings
    {
        public RvzFormat Format { get; set; } = RvzFormat.Rvz;
        /// <summary>WIA and RVZ only. RVZ can't use Purge; WIA can't use Zstd.</summary>
        public RvzCompression Compression { get; set; } = RvzCompression.Zstd;
        /// <summary>See <see cref="DolphinRvz.CompressionLevels"/>.</summary>
        public int CompressionLevel { get; set; } = 5;
        /// <summary>GCZ, WIA and RVZ; see <see cref="DolphinRvz.IsBlockSizeValid"/>.</summary>
        public uint BlockSize { get; set; } = 0x20000;
        /// <summary>Zero unused areas of the disc first (makes the image no longer match Redump).</summary>
        public bool Scrub { get; set; }
        /// <summary>Replace an existing output file.</summary>
        public bool Overwrite { get; set; }
    }

    /// <summary>
    /// One libdolphinrvz context. Operations block; run them on a worker
    /// thread. An instance runs one operation at a time; use separate
    /// instances for parallel work. Every operation accepts a
    /// CancellationToken; a cancelled conversion deletes its partial output
    /// and throws OperationCanceledException.
    /// </summary>
    public sealed class DolphinRvz : IDisposable
    {
        readonly ContextHandle _ctx;
        readonly ProgressCallback _progressCallback; // referenced here so the GC keeps them alive
        readonly LogCallback _logCallback;
        int _busy;
        CancellationToken _token;
        ExceptionDispatchInfo? _callbackError;

        /// <summary>Receives progress reports on the calling thread.</summary>
        public IProgress<DrvzProgress>? Progress { get; set; }

        /// <summary>Receives DolphinTool's warnings and Dolphin's errors on the calling thread.</summary>
        public Action<DrvzLogLevel, string>? Log { get; set; }

        /// <param name="threads">Compression worker threads; 0 = all processors.</param>
        public DolphinRvz(int threads = 0)
        {
            Check(Native.drvz_context_create(out _ctx), null);
            _progressCallback = OnNativeProgress;
            _logCallback = OnNativeLog;
            Native.drvz_context_set_progress(_ctx, _progressCallback, IntPtr.Zero);
            Native.drvz_context_set_log(_ctx, _logCallback, IntPtr.Zero);
            Native.drvz_context_set_num_threads(_ctx, threads);
        }

        public static string Version => Native.Str(Native.drvz_version());
        public static int AbiVersion => Native.drvz_abi_version();

        /// <summary>Whether <paramref name="blockSize"/> is allowed for <paramref name="format"/> (ISO: only 0).</summary>
        public static bool IsBlockSizeValid(RvzFormat format, uint blockSize) => Native.drvz_block_size_valid(format, blockSize) != 0;

        /// <summary>The levels Dolphin allows (0..0 for None and Purge).</summary>
        public static (int Min, int Max) CompressionLevels(RvzCompression compression)
        {
            Native.drvz_compression_levels(compression, out int min, out int max);
            return (min, max);
        }

        /// <summary>Cancels the running operation from any thread. A cancel just before an operation starts cancels that operation.</summary>
        public void Cancel()
        {
            try
            {
                if (!_ctx.IsClosed)
                    Native.drvz_context_cancel(_ctx);
            }
            catch (ObjectDisposedException)
            {
                // disposed concurrently: nothing left to cancel
            }
        }

        public void Dispose() => _ctx.Dispose();

        /// <summary>Reads what kind of GameCube/Wii image <paramref name="path"/> is. Throws with <see cref="DrvzStatus.NotADisc"/> for anything else.</summary>
        public DiscInfo Probe(string path, CancellationToken cancellationToken = default)
        {
            var n = NewDiscInfo();
            Run(cancellationToken, () => Native.drvz_probe(_ctx, path, ref n));
            return new DiscInfo(
                n.Container, n.Platform, Native.Str(n.GameId), Native.Str(n.MakerId), Native.Str(n.InternalName),
                n.DiscNumber, n.Revision, n.Region, n.DataSize, n.DataSizeIsExact != 0, n.FileSize, n.BlockSize,
                Native.Str(n.CompressionMethod), n.HasCompressionLevel != 0 ? n.CompressionLevel : null,
                n.IsNKit != 0, n.IsDatel != 0, n.IsScrubbable != 0);
        }

        /// <summary>Converts <paramref name="input"/> to <paramref name="output"/>. The output appears only when complete.</summary>
        public void Convert(string input, string output, ConvertSettings settings, CancellationToken cancellationToken = default)
        {
            var o = new ConvertOptionsNative();
            Native.drvz_convert_options_init(ref o);
            o.Input = input;
            o.Output = output;
            o.Format = settings.Format;
            o.Compression = settings.Compression;
            o.CompressionLevel = settings.CompressionLevel;
            o.BlockSize = settings.Format == RvzFormat.Iso ? 0 : settings.BlockSize;
            o.Scrub = settings.Scrub ? 1 : 0;
            o.Overwrite = settings.Overwrite ? 1 : 0;
            Run(cancellationToken, () => Native.drvz_convert(_ctx, ref o));
        }

        /// <summary>CRC32, MD5 and SHA-1 of the decoded disc (the bytes a plain ISO of it holds).</summary>
        public DiscHashes Hash(string path, CancellationToken cancellationToken = default)
        {
            var n = new HashResultNative { Crc32 = new byte[9], Md5 = new byte[33], Sha1 = new byte[41] };
            Native.drvz_hash_result_init(ref n);
            Run(cancellationToken, () => Native.drvz_hash(_ctx, path, ref n));
            return new DiscHashes(Native.Str(n.Crc32), Native.Str(n.Md5), Native.Str(n.Sha1), n.Size, n.SizeIsExact != 0);
        }

        static DiscInfoNative NewDiscInfo()
        {
            var n = new DiscInfoNative
            {
                GameId = new byte[8], MakerId = new byte[4], InternalName = new byte[256], CompressionMethod = new byte[32],
            };
            Native.drvz_disc_info_init(ref n);
            return n;
        }

        /// <summary>The managed struct sizes, which must equal the C sizeof() values (for tests).</summary>
        public static (int DiscInfo, int ConvertOptions, int HashResult) StructSizes =>
            (Marshal.SizeOf<DiscInfoNative>(), Marshal.SizeOf<ConvertOptionsNative>(), Marshal.SizeOf<HashResultNative>());

        public static (int DiscInfo, int ConvertOptions, int HashResult) ExpectedStructSizes =>
            (Native.DiscInfoSize, Native.ConvertOptionsSize, Native.HashResultSize);

        // ------------------------------------------------------- internals

        delegate DrvzStatus NativeCall();

        void Run(CancellationToken token, NativeCall call)
        {
            if (Interlocked.Exchange(ref _busy, 1) != 0)
                throw new InvalidOperationException("This DolphinRvz instance is already running an operation; use one instance per concurrent operation.");
            try
            {
                ObjectDisposedException.ThrowIf(_ctx.IsClosed, this);
                token.ThrowIfCancellationRequested();
                _token = token;
                _callbackError = null;
                DrvzStatus status = call();
                _callbackError?.Throw();
                if (status == DrvzStatus.Cancelled)
                    token.ThrowIfCancellationRequested();
                Check(status, _ctx);
            }
            finally
            {
                _token = default;
                _callbackError = null;
                Volatile.Write(ref _busy, 0);
            }
        }

        static void Check(DrvzStatus status, ContextHandle? ctx)
        {
            if (status == DrvzStatus.Ok)
                return;
            string message = ctx is { IsInvalid: false, IsClosed: false } ? Native.Str(Native.drvz_last_error(ctx)) : "";
            if (message.Length == 0)
                message = Native.Str(Native.drvz_status_string(status));
            throw new DolphinRvzException(status, message);
        }

        // Callbacks run on the operation's thread. Exceptions must not cross
        // into native code: they cancel the operation and are rethrown by Run.
        int OnNativeProgress(IntPtr user, DrvzPhase phase, double fraction, double ratio)
        {
            if (_callbackError != null)
                return 1;
            try
            {
                Progress?.Report(new DrvzProgress(phase, fraction, ratio));
                return _token.IsCancellationRequested ? 1 : 0;
            }
            catch (Exception ex)
            {
                _callbackError = ExceptionDispatchInfo.Capture(ex);
                return 1;
            }
        }

        void OnNativeLog(IntPtr user, DrvzLogLevel level, IntPtr message)
        {
            if (_callbackError != null)
                return;
            try
            {
                Log?.Invoke(level, Native.Str(message));
            }
            catch (Exception ex)
            {
                _callbackError = ExceptionDispatchInfo.Capture(ex);
            }
        }
    }
}
