#include "network_settings.h"
#include "connectivity_runtime.h"
#include "backup_store.h"
#include "backup_transport.h"
#include "sd_card.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <iostream>
#include <future>
#include <chrono>

static NetworkSettings example() {
    NetworkSettings s;
    s.wifi_count=1;s.wifi[0]={"Home","wifi-password"};
    s.setup_password="my-camera-setup";
    s.backup_enabled=true;s.url="https://example.test/dav/photos";
    s.username="person";s.password="app-password";
    return s;
}
int main(int argc,char **argv) {
    assert(argc==2);
    const std::string scenario=argv[1];
    NetworkSettings s=example(),out;
    std::string error;
    std::vector<uint8_t> image(1301);
    for (size_t i=0;i<image.size();++i) image[i]=uint8_t(i);
    const std::string digest=backupDigest(image.data(),image.size());
    const std::string destination=backupDestinationId(s);
    if (scenario=="settings_roundtrip") {
        assert(decodeSettings(encodeSettings(s,true),out,error));
        assert(out.wifi[0].password==s.wifi[0].password && out.password==s.password);
        const auto redacted=encodeSettings(s,false);
        assert(redacted.find("wifi-password")==std::string::npos);
        assert(redacted.find("app-password")==std::string::npos);
        assert(redacted.find("my-camera-setup")==std::string::npos);
    } else if (scenario=="settings_preserve_secrets") {
        assert(applySettingsForm(R"({"wifi":{"ssid":"Home","password":""},"backup":{"enabled":true,"url":"https://example.test/dav/photos/","username":"person"}})",s,out,error));
        assert(out.password==s.password && out.wifi[0].password==s.wifi[0].password);
    } else if (scenario=="settings_destination_change") {
        assert(applySettingsForm(R"({"backup":{"enabled":true,"url":"https://other.test/dav","username":"person"}})",s,out,error));
        assert(out.password.empty());
        assert(backupDestinationId(out)!=destination);
    } else if (scenario=="settings_profiles") {
        s.wifi_count=3;s.wifi[1]={"Office","office-pass"};s.wifi[2]={"Old","old-password"};
        assert(applySettingsForm(R"({"wifi":{"ssid":"Phone","password":"phone-pass"}})",s,out,error));
        assert(out.wifi_count==3 && out.wifi[0].ssid=="Phone" && out.wifi[1].ssid=="Home" && out.wifi[2].ssid=="Office");
        assert(applySettingsForm(R"({"wifi":{"ssid":"Home","password":"ignored-password","open":true},"forget_others":true})",s,out,error));
        assert(out.wifi_count==1 && out.wifi[0].password.empty());
    } else if (scenario=="settings_validation") {
        assert(!applySettingsForm(R"({"backup":{"enabled":true,"url":"http://example.test/dav"}})",s,out,error));
        assert(!applySettingsForm(R"({"backup":{"enabled":true,"url":"https://user:secret@example.test/dav"}})",s,out,error));
        assert(!applySettingsForm(R"({"backup":{"provider":"google-drive"}})",s,out,error));
        assert(!applySettingsForm(R"({"wifi":{"ssid":"Home","password":"short"}})",s,out,error));
        assert(!applySettingsForm(R"({"wifi":{"ssid":"Unknown"}})",s,out,error));
        assert(!applySettingsForm(std::string(9000,'x'),s,out,error));
        assert(!applySettingsForm("{",s,out,error));
        assert(!applySettingsForm("{}",NetworkSettings{},out,error));
    } else if (scenario=="retry_clock_wrap") {
        RetrySchedule retry;
        const uint32_t start=UINT32_MAX-2000;
        retry.fail(start);assert(!retry.ready(uint32_t(start+4999)));
        assert(retry.ready(uint32_t(start+5000)));
        retry.fail(10000);assert(!retry.ready(19999));assert(retry.ready(20000));
        retry.reset();assert(retry.ready(0));
    } else if (scenario=="sha256") {
        assert(backupDigest(reinterpret_cast<const uint8_t *>("abc"),3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    } else if (scenario=="snapshot_and_receipt") {
        assert(initSDCard());mock_files["/1.jpg"]=image;
        PhotoSnapshot photo;assert(nextBackupPhoto("",photo)==SnapshotResult::Loaded);
        assert(photo.source=="/1.jpg" && photo.size==image.size());
        assert(!hasBackupReceipt(destination,digest));
        assert(writeBackupReceipt(photo,destination,digest));
        assert(hasBackupReceipt(destination,digest));
        NetworkSettings other=s;other.url="https://other.test/dav";
        assert(!hasBackupReceipt(backupDestinationId(other),digest));
        assert(nextBackupPhoto("/1.jpg",photo)==SnapshotResult::End);
    } else if (scenario=="card_swap") {
        assert(initSDCard());mock_files["/1.jpg"]=image;
        PhotoSnapshot photo;assert(nextBackupPhoto("",photo)==SnapshotResult::Loaded);
        assert(initSDCard());
        assert(!writeBackupReceipt(photo,destination,digest));
        assert(!hasBackupReceipt(destination,digest));
    } else if (scenario=="snapshot_limits") {
        assert(initSDCard());mock_files["/1.jpg"]=std::vector<uint8_t>(2*1024*1024+1);
        PhotoSnapshot photo;assert(nextBackupPhoto("",photo)==SnapshotResult::Skipped);
        mock_files["/1.jpg"]=image;mock_allocation_ok=false;
        assert(nextBackupPhoto("",photo)==SnapshotResult::Unavailable);
        mock_allocation_ok=true;mock_read_limit=0;
        assert(nextBackupPhoto("",photo)==SnapshotResult::Skipped);
    } else if (scenario=="snapshot_remount_midread" || scenario=="snapshot_capture_midread") {
        assert(initSDCard());
        mock_files["/1.jpg"]=std::vector<uint8_t>(40000,42);
        mock_delay_hook=[&] {
            mock_delay_hook=nullptr;
            if (scenario=="snapshot_remount_midread") assert(initSDCard());
            else {
                auto capture=std::async(std::launch::async,[&]{ return savePhotoToSD(image.data(),image.size()); });
                assert(capture.wait_for(std::chrono::seconds(2))==std::future_status::ready);
                assert(capture.get());
            }
        };
        PhotoSnapshot photo;
        const auto result=nextBackupPhoto("",photo);
        if (scenario=="snapshot_remount_midread") assert(result==SnapshotResult::Unavailable);
        else { assert(result==SnapshotResult::Loaded);assert(mock_files.count("/2.jpg")); }
    } else if (scenario=="receipt_write_failure") {
        assert(initSDCard());mock_files["/1.jpg"]=image;
        PhotoSnapshot photo;assert(nextBackupPhoto("",photo)==SnapshotResult::Loaded);
        mock_write_limit=2;
        assert(!writeBackupReceipt(photo,destination,digest));
        assert(!hasBackupReceipt(destination,digest));
    } else if (scenario=="remote_roundtrip") {
        assert(uploadBackup(s,image.data(),image.size(),digest,error));
        assert(mock_remote==image && mock_get_calls==1 && mock_put_calls==1);
        assert(mock_last_url==s.url+"/"+digest+".jpg");
    } else if (scenario=="remote_corrupt" || scenario=="remote_short") {
        mock_remote_corrupt=scenario=="remote_corrupt";
        mock_remote_short=scenario=="remote_short";
        assert(!uploadBackup(s,image.data(),image.size(),digest,error));
        assert(!error.empty());
    } else if (scenario=="http_auth_failure" || scenario=="http_redirect") {
        mock_put_status=scenario=="http_auth_failure" ? 401 : 302;
        assert(!uploadBackup(s,image.data(),image.size(),digest,error));
        assert(mock_get_calls==0);
    } else if (scenario=="lost_ack_idempotent") {
        mock_http_reply_lost=true;
        assert(!uploadBackup(s,image.data(),image.size(),digest,error));
        const auto url=mock_last_url;
        mock_http_reply_lost=false;
        assert(uploadBackup(s,image.data(),image.size(),digest,error));
        assert(mock_last_url==url && mock_remote==image && mock_put_calls==2);
    } else if (scenario=="capture_during_http") {
        assert(initSDCard());mock_files["/1.jpg"]=image;
        PhotoSnapshot photo;assert(nextBackupPhoto("",photo)==SnapshotResult::Loaded);
        mock_on_http=[&] {
            // HTTP中に別タスクがSDに書けること
            auto capture=std::async(std::launch::async,[&]{ return savePhotoToSD(image.data(),image.size()); });
            assert(capture.wait_for(std::chrono::seconds(2))==std::future_status::ready);
            assert(capture.get());
        };
        assert(uploadBackup(s,photo.data.get(),photo.size,digest,error));
        assert(mock_files.count("/2.jpg") && mock_files.count("/3.jpg"));
    } else return 2;
    std::cout<<"PASS "<<scenario<<'\n';
}
