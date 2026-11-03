using System.IO;

namespace Orders
{
    /// <summary>Orders are kept as one file per region: &lt;Orders:DataDir&gt;/&lt;region&gt;.orders</summary>
    public class OrderStore
    {
        private readonly string dataDir;

        public OrderStore(string dataDir)
        {
            this.dataDir = dataDir;
        }

        public int CountOrders(string region, int batchSize)
        {
            string file = Path.Combine(dataDir, region + ".orders");
            if (!File.Exists(file))
            {
                throw new FileNotFoundException($"Order file for region {region} not found", file);
            }

            int count = 0;
            foreach (string line in File.ReadLines(file))
            {
                if (line.Length > 0)
                {
                    ++count;
                }
            }

            return count;
        }
    }
}
