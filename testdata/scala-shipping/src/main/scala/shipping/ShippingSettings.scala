package shipping

import com.typesafe.config.Config

import java.nio.file.{Files, NoSuchFileException, Path, Paths}

// Scala 3 braceless style.
final case class ShippingSettings(dataDir: String, maxParcels: Int)

object ShippingSettings:
  def from(config: Config): ShippingSettings =
    val dataDir = sys.env.getOrElse("SHIPPING_DATA_DIR", config.getString("shipping.data-dir"))
    val maxParcels = config.getInt("shipping.max-parcels")
    if maxParcels <= 0 then
      throw new IllegalArgumentException(s"shipping.max-parcels must be positive, got $maxParcels")
    ShippingSettings(dataDir, maxParcels)

class ParcelStore(dataDir: String):
  def countParcels(zone: String, maxParcels: Int): Int =
    val file = Paths.get(dataDir).resolve(s"$zone.parcels")
    if !Files.isRegularFile(file) then
      throw new NoSuchFileException(file.toString, null, s"parcel file for zone $zone not found")
    Files.readAllLines(file).size.min(maxParcels)
end ParcelStore
