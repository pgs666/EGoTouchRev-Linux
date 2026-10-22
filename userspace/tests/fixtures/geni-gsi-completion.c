/* Test fixture: exact GSI completion callback from local 7.1.8-gaokun3+.
 * Full source SHA-256:
 * edd0d278011d2200b9a2ee84b4ea923ed4605b4b61f25997c15e8d494348af61
 * Original source: drivers/spi/spi-geni-qcom.c, SPDX-License-Identifier: GPL-2.0.
 * Keep this unmodified; the regression test applies the shipped kernel patch.
 */
static void
spi_gsi_callback_result(void *cb, const struct dmaengine_result *result)
{
	struct spi_controller *spi = cb;

	spi->cur_msg->status = -EIO;
	if (result->result != DMA_TRANS_NOERROR) {
		dev_err(&spi->dev, "DMA txn failed: %d\n", result->result);
		spi_finalize_current_transfer(spi);
		return;
	}

	if (!result->residue) {
		spi->cur_msg->status = 0;
		dev_dbg(&spi->dev, "DMA txn completed\n");
	} else {
		dev_err(&spi->dev, "DMA xfer has pending: %d\n", result->residue);
	}

	spi_finalize_current_transfer(spi);
}
