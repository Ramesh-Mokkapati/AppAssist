package com.example.inventory;

/** The settings the report runs with. */
public final class AppConfig
{
    private final String dataDir;
    private final int pageSize;

    public AppConfig(String dataDir, int pageSize)
    {
        this.dataDir = dataDir;
        this.pageSize = pageSize;
    }

    public String dataDir()
    {
        return dataDir;
    }

    public int pageSize()
    {
        return pageSize;
    }
}
