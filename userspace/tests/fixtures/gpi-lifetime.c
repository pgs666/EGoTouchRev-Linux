/* Exact local 7.1.8-gaokun3+ function bodies, GPL-2.0-only.
 * drivers/dma/qcom/gpi.c SHA-256:
 * a89b8e2e2e3b4f38d607700d461f814affc4c2614713c951c14388f273499a5a
 * Preserve this fixture; the regression runner applies the shipped patch.
 */
static int
gpi_peripheral_config(struct dma_chan *chan, struct dma_slave_config *config)
{
	struct gchan *gchan = to_gchan(chan);
	void *new_config;

	if (!config->peripheral_config)
		return -EINVAL;

	new_config = krealloc(gchan->config, config->peripheral_size, GFP_NOWAIT);
	if (!new_config)
		return -ENOMEM;

	gchan->config = new_config;
	memcpy(gchan->config, config->peripheral_config, config->peripheral_size);

	return 0;
}

static void gpi_free_chan_resources(struct dma_chan *chan)
{
	struct gchan *gchan = to_gchan(chan);
	struct gpii *gpii = gchan->gpii;
	enum gpi_pm_state cur_state;
	int ret, i;

	mutex_lock(&gpii->ctrl_lock);

	cur_state = gchan->pm_state;

	/* disable ch state so no more TRE processing for this channel */
	write_lock_irq(&gpii->pm_lock);
	gchan->pm_state = PREPARE_TERMINATE;
	write_unlock_irq(&gpii->pm_lock);

	/* attempt to do graceful hardware shutdown */
	if (cur_state == ACTIVE_STATE) {
		gpi_stop_chan(gchan);

		ret = gpi_send_cmd(gpii, gchan, GPI_CH_CMD_RESET);
		if (ret)
			dev_err(gpii->gpi_dev->dev, "error resetting channel:%d\n", ret);

		gpi_reset_chan(gchan, GPI_CH_CMD_DE_ALLOC);
	}

	/* free all allocated memory */
	gpi_free_ring(&gchan->ch_ring, gpii);
	vchan_free_chan_resources(&gchan->vc);
	kfree(gchan->config);

	write_lock_irq(&gpii->pm_lock);
	gchan->pm_state = DISABLE_STATE;
	write_unlock_irq(&gpii->pm_lock);

	/* if other rings are still active exit */
	for (i = 0; i < MAX_CHANNELS_PER_GPII; i++)
		if (gpii->gchan[i].ch_ring.configured)
			goto exit_free;

	/* deallocate EV Ring */
	cur_state = gpii->pm_state;
	write_lock_irq(&gpii->pm_lock);
	gpii->pm_state = PREPARE_TERMINATE;
	write_unlock_irq(&gpii->pm_lock);

	/* wait for threads to complete out */
	tasklet_kill(&gpii->ev_task);

	/* send command to de allocate event ring */
	if (cur_state == ACTIVE_STATE)
		gpi_send_cmd(gpii, NULL, GPI_EV_CMD_DEALLOC);

	gpi_free_ring(&gpii->ev_ring, gpii);

	/* disable interrupts */
	if (cur_state == ACTIVE_STATE)
		gpi_disable_interrupts(gpii);

	/* set final state to disable */
	write_lock_irq(&gpii->pm_lock);
	gpii->pm_state = DISABLE_STATE;
	write_unlock_irq(&gpii->pm_lock);

exit_free:
	mutex_unlock(&gpii->ctrl_lock);
}
