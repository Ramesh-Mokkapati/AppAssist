package com.example.inventory;

import java.io.IOException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * A small stock-report tool used to test AppAssist on Java code. It reads its settings from
 * config/inventory.properties, system properties, the environment and the command line, and fails in
 * realistic ways when they are wrong.
 */
public class InventoryApp
{
    private static final Logger LOG = Logger.getLogger("inventory");
    static final int MAX_RETRIES = 2;

    public static void main(String[] args)
    {
        Path configFile = Paths.get("config", "inventory.properties");
        String warehouse = "main";
        for (int i = 0; i < args.length; ++i)
        {
            if ("--config".equals(args[i]) && i + 1 < args.length)
            {
                configFile = Paths.get(args[++i]);
            }
            else if ("--warehouse".equals(args[i]) && i + 1 < args.length)
            {
                warehouse = args[++i];
            }
        }

        LOG.info("Inventory report starting for warehouse " + warehouse);
        InventoryApp app = new InventoryApp();
        app.run(configFile, warehouse);
    }

    void run(Path configFile, String warehouse)
    {
        AppConfig config;
        try
        {
            config = ConfigLoader.load(configFile);
        }
        catch (IOException e)
        {
            LOG.log(Level.SEVERE, "Cannot read the configuration file " + configFile, e);
            return;
        }

        StockRepository repository = new StockRepository(config.dataDir());
        int attempt = 0;
        while (true)
        {
            try
            {
                int items = repository.countItems(warehouse, config.pageSize());
                LOG.info("Warehouse " + warehouse + " has " + items + " items");
                return;
            }
            catch (IOException e)
            {
                ++attempt;
                if (attempt > MAX_RETRIES)
                {
                    LOG.log(Level.SEVERE, "Giving up on warehouse " + warehouse + " after " + attempt + " attempts", e);
                    return;
                }

                LOG.warning("Stock file not readable, retrying (attempt " + attempt + ")");
            }
        }
    }
}
