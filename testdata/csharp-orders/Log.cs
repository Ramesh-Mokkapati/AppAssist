using System;

namespace Orders
{
    /// <summary>Writes "2026-11-03 14:20:01.123 ERROR orders: message" lines to standard error.</summary>
    public static class Log
    {
        public static void Info(string message) => Write("INFO ", message, null);

        public static void Warn(string message) => Write("WARN ", message, null);

        public static void Error(string message, Exception? error) => Write("ERROR", message, error);

        private static void Write(string level, string message, Exception? error)
        {
            Console.Error.WriteLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} {level} orders: {message}");
            if (error != null)
            {
                Console.Error.WriteLine(error.ToString());
            }
        }
    }
}
