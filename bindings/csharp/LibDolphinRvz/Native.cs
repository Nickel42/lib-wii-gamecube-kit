// Raw P/Invoke layer for libdolphinrvz (dolphinrvz.h). Struct layouts mirror
// the C header exactly; the public API lives in DolphinRvz.cs.
// SPDX-License-Identifier: GPL-2.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace LibDolphinRvz.Interop
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int ProgressCallback(IntPtr user, DrvzPhase phase, double fraction, double ratio);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void LogCallback(IntPtr user, DrvzLogLevel level, IntPtr message);

    /// <summary>Owns a drvz_context. Passing it to a native call keeps it alive for the call's duration.</summary>
    internal sealed class ContextHandle : SafeHandle
    {
        public ContextHandle() : base(IntPtr.Zero, ownsHandle: true) { }
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle()
        {
            Native.drvz_context_destroy(handle);
            return true;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DiscInfoNative
    {
        public nuint StructSize;
        public DiscContainer Container;
        public DiscPlatform Platform;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public byte[] GameId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public byte[] MakerId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] InternalName;
        public int DiscNumber;
        public int Revision;
        public DiscRegion Region;
        public ulong DataSize;
        public ulong FileSize;
        public uint BlockSize;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] CompressionMethod;
        public int CompressionLevel;
        public int HasCompressionLevel;
        public int IsNKit;
        public int IsDatel;
        public int IsScrubbable;
        public int DataSizeIsExact;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ConvertOptionsNative
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Output;
        public RvzFormat Format;
        public RvzCompression Compression;
        public int CompressionLevel;
        public uint BlockSize;
        public int Scrub;
        public int Overwrite;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct HashResultNative
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 9)] public byte[] Crc32;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 33)] public byte[] Md5;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 41)] public byte[] Sha1;
        public ulong Size;
        public int SizeIsExact;
    }

    internal static class Native
    {
        // dolphinrvz.dll, libdolphinrvz.dylib or libdolphinrvz.so.
        const string Lib = "dolphinrvz";

        /// <summary>sizeof() of the C structs on 64-bit platforms (checked by the tests on both sides).</summary>
        public const int DiscInfoSize = 376, ConvertOptionsSize = 48, HashResultSize = 112;

        public static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "";

        /// <summary>A NUL-terminated UTF-8 string from a fixed-size char array.</summary>
        public static string Str(byte[]? bytes)
        {
            if (bytes == null) return "";
            int n = Array.IndexOf(bytes, (byte)0);
            return Encoding.UTF8.GetString(bytes, 0, n < 0 ? bytes.Length : n);
        }

        [DllImport(Lib)] public static extern IntPtr drvz_status_string(DrvzStatus status);
        [DllImport(Lib)] public static extern IntPtr drvz_version();
        [DllImport(Lib)] public static extern int drvz_abi_version();

        [DllImport(Lib)] public static extern DrvzStatus drvz_context_create(out ContextHandle ctx);
        [DllImport(Lib)] public static extern void drvz_context_destroy(IntPtr ctx);
        [DllImport(Lib)] public static extern void drvz_context_set_progress(ContextHandle ctx, ProgressCallback? fn, IntPtr user);
        [DllImport(Lib)] public static extern void drvz_context_set_log(ContextHandle ctx, LogCallback? fn, IntPtr user);
        [DllImport(Lib)] public static extern void drvz_context_set_num_threads(ContextHandle ctx, int count);
        [DllImport(Lib)] public static extern void drvz_context_cancel(ContextHandle ctx);
        [DllImport(Lib)] public static extern IntPtr drvz_last_error(ContextHandle ctx);

        [DllImport(Lib)] public static extern void drvz_disc_info_init(ref DiscInfoNative info);
        [DllImport(Lib)] public static extern DrvzStatus drvz_probe(ContextHandle ctx, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, ref DiscInfoNative info);

        [DllImport(Lib)] public static extern void drvz_convert_options_init(ref ConvertOptionsNative opts);
        [DllImport(Lib)] public static extern DrvzStatus drvz_convert(ContextHandle ctx, [In] ref ConvertOptionsNative opts);
        [DllImport(Lib)] public static extern int drvz_block_size_valid(RvzFormat format, uint blockSize);
        [DllImport(Lib)] public static extern void drvz_compression_levels(RvzCompression compression, out int min, out int max);

        [DllImport(Lib)] public static extern void drvz_hash_result_init(ref HashResultNative result);
        [DllImport(Lib)] public static extern DrvzStatus drvz_hash(ContextHandle ctx, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, ref HashResultNative result);
    }
}
