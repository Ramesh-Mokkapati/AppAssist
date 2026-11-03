package com.example.inventory;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

/** Reads config/inventory.properties; system properties and the environment override it. */
public final class ConfigLoader
{
    private ConfigLoader()
    {
    }

    public static AppConfig load(Path file) throws IOException
    {
        Properties props = new Properties();
        if (Files.exists(file))
        {
            try (InputStream in = Files.newInputStream(file))
            {
                props.load(in);
            }
        }

        String dataDir = System.getenv("INVENTORY_DATA_DIR");
        if (dataDir == null)
        {
            dataDir = props.getProperty("data.dir", "data");
        }

        String pageSizeText = System.getProperty("inventory.pageSize", props.getProperty("page.size", "50"));
        int pageSize = parsePageSize(pageSizeText);
        return new AppConfig(dataDir, pageSize);
    }

    static int parsePageSize(String text)
    {
        int value = Integer.parseInt(text.trim());
        if (value <= 0)
        {
            throw new IllegalArgumentException("page.size must be positive, got " + value);
        }

        return value;
    }
}
