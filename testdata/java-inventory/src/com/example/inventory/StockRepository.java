package com.example.inventory;

import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;

/** Stock is kept as one CSV file per warehouse: <data.dir>/<warehouse>.csv */
public class StockRepository
{
    private final Path dataDir;

    public StockRepository(String dataDir)
    {
        this.dataDir = Paths.get(dataDir);
    }

    public int countItems(String warehouse, int pageSize) throws IOException
    {
        Path file = dataDir.resolve(warehouse + ".csv");
        if (!Files.isRegularFile(file))
        {
            throw new NoSuchFileException(file.toString(), null, "stock file for warehouse " + warehouse + " not found");
        }

        int count = 0;
        try (BufferedReader reader = Files.newBufferedReader(file, StandardCharsets.UTF_8))
        {
            String line;
            while ((line = reader.readLine()) != null)
            {
                if (!line.isEmpty())
                {
                    ++count;
                }
            }
        }

        return count;
    }
}
