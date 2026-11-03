using System;
using System.IO;

namespace Orders
{
    /// <summary>
    /// A small order-report tool used to test AppAssist on C# code. Settings come from appsettings.json,
    /// environment variables and the command line.
    /// </summary>
    public static class Program
    {
        public const int MaxRetries = 2;

        public static int Main(string[] args)
        {
            string settingsFile = Path.Combine(AppContext.BaseDirectory, "appsettings.json");
            string region = "west";
            for (int i = 0; i < args.Length; ++i)
            {
                if (args[i] == "--settings" && i + 1 < args.Length)
                {
                    settingsFile = args[++i];
                }
                else if (args[i] == "--region" && i + 1 < args.Length)
                {
                    region = args[++i];
                }
            }

            Log.Info("Order report starting for region " + region);
            AppSettings settings = AppSettings.Load(settingsFile);
            int batchSize = ReadBatchSize(settings);
            return Run(settings, region, batchSize);
        }

        static int ReadBatchSize(AppSettings settings)
        {
            string? text = Environment.GetEnvironmentVariable("ORDERS_BATCH_SIZE") ?? settings["Orders:BatchSize"];
            int value = int.Parse(text ?? "100");
            if (value <= 0)
            {
                throw new ArgumentOutOfRangeException("Orders:BatchSize", value, "Orders:BatchSize must be positive");
            }

            return value;
        }

        static int Run(AppSettings settings, string region, int batchSize)
        {
            string dataDir = Environment.GetEnvironmentVariable("ORDERS_DATA_DIR") ?? settings["Orders:DataDir"] ?? "data";
            OrderStore store = new OrderStore(dataDir);
            int attempt = 0;
            while (true)
            {
                try
                {
                    int orders = store.CountOrders(region, batchSize);
                    Log.Info($"Region {region} has {orders} orders");
                    return 0;
                }
                catch (IOException e)
                {
                    ++attempt;
                    if (attempt > MaxRetries)
                    {
                        Log.Error($"Giving up on region {region} after {attempt} attempts", e);
                        return 1;
                    }

                    Log.Warn($"Order file not readable, retrying (attempt {attempt})");
                }
            }
        }
    }
}
