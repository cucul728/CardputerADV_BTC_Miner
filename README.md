Cardputer ADV – Solo BTC Miner
by Şükrü Vayıç

M5Stack Cardputer ADV (ESP32-S3) için hazırlanmış, WiFi sihirbazlı bir solo Bitcoin "piyango" madencisi. Cihazın klavyesi ve ekranıyla WiFi ağını seçersin, şifreyi yazarsın, madencilik kendiliğinden başlar.

⚠️ Önemli: Varsayılan BTC adresi
Bu yazılımda varsayılan cüzdan adresi geliştiricinin (benim) BTC adresidir:

1KJKzc3YknKSPyjybgx6GzkvKZjo9c7rWV
Adresini değiştirmezsen cihazın bulduğu olası bir blok ödülü bana gider. Kendi adresini kullanmak için aşağıdaki "Kendi BTC adresini ayarlama" bölümüne bak.

Gerçekçi beklenti
Hız yaklaşık 30–35 KH/s civarındadır.
Bitcoin ağının toplam hızı yüzlerce EH/s seviyesindedir. Blok bulma ihtimali pratikte sıfıra çok yakındır.
Bu proje eğlence ve öğrenme amaçlıdır, para kazanma aracı değildir.
Pool zorluğu 1.00 iken ekranda "Shares 0/0" görmek uzun süre normaldir.
Kurulum (derlemeye gerek yok)
Releases sayfasından BTC_Miner.bin dosyasını indir.
Dosyayı FAT32 biçimli bir SD kartın kök dizinine kopyala.
SD kartı Cardputer ADV'ye tak.
Cihazda M5 Launcher ile aç → SD menüsü → BTC_Miner.bin → Install.
Yükleme bitince cihaz yeniden başlar ve program açılır.
Launcher yüklü değilse önce M5Burner üzerinden Launcher firmware'ini yükle.

İlk kullanım
Açılışta WiFi sihirbazı çevredeki ağları tarar ve listeler.
; / . tuşlarıyla ağı seç, Enter'a bas.
Şifreyi yaz, Enter. Bağlanırsa ağ otomatik kaydedilir.
Madencilik başlar. Sonraki açılışlarda kayıtlı ağlara kendisi bağlanır (4 ağa kadar).
Tuşlar
Tuş	İşlev
Tab veya /	Sonraki sayfa
,	Önceki sayfa
1 2 3	Sayfaya git
W	WiFi seç / kur
P veya BtnG0	Duraklat / devam
- =	Ekran parlaklığı
; .	Listede yukarı / aşağı
Enter	Seç
Del veya `	Geri
WiFi listesi: R yeniden tara, D kayıtlı ağı unut. Yazı girişi: Del sil, Tab şifreyi göster/gizle, Enter tamam, Fn + ` iptal, Aa + harf = büyük harf.

Kendi BTC adresini ayarlama
3 tuşuyla Ayarlar sayfasına git.
; / . ile BTC adres satırını seç, Enter'a bas.
Kendi cüzdan adresini yaz, Enter ile kaydet.
Aynı sayfadan Pool host, Pool port ve Worker adını da değiştirebilirsin.

Ayar	Varsayılan
Pool	public-pool.io:21496
Yedek pool	solo.ckpool.org
Worker	cardputer
Kaynaktan derleme (isteğe bağlı)
Kütüphaneler: M5Cardputer, ArduinoJson 7.x

Kart ayarları: M5Stack → M5Cardputer, USB CDC On Boot: Enabled, Partition: 8M with spiffs

arduino-cli compile \
  --fqbn m5stack:esp32:m5stack_cardputer:CDCOnBoot=cdc,PartitionScheme=default_8MB \
  --output-dir build .
SD karttan Launcher ile yüklemek için build/*.ino.bin dosyasını kullan.

Sorumluluk reddi
Yazılım "olduğu gibi" sunulur, garanti verilmez. Kullanım kendi sorumluluğundadır. Cihazın aşırı ısınmasına dikkat et (normal sıcaklık yaklaşık 50–55 °C).

Lisans
MIT License

by Şükrü Vayıç
