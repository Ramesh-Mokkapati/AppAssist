package shipping

import com.typesafe.config.{Config, ConfigFactory}
import org.slf4j.LoggerFactory

import java.io.IOException

// Scala 2 brace style.
object ShippingApp {
  private val log = LoggerFactory.getLogger("shipping")
  final val MaxRetries = 2

  def main(args: Array[String]): Unit = {
    var zone = "north"
    args.toList match {
      case "--zone" :: z :: _ => zone = z
      case _ =>
    }

    log.info(s"Shipping report starting for zone $zone")
    val config = ConfigFactory.load()
    val settings = ShippingSettings.from(config)
    run(settings, zone)
  }

  def run(settings: ShippingSettings, zone: String): Unit = {
    val store = new ParcelStore(settings.dataDir)
    var attempt = 0
    while (true) {
      try {
        val parcels = store.countParcels(zone, settings.maxParcels)
        log.info(s"Zone $zone has $parcels parcels")
        return
      } catch {
        case e: IOException =>
          attempt += 1
          if (attempt > MaxRetries) {
            log.error(s"Giving up on zone $zone after $attempt attempts", e)
            return
          }
          log.warn(s"Parcel file not readable, retrying (attempt $attempt)")
      }
    }
  }
}
