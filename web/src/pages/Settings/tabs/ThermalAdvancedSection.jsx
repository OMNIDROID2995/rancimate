import { Trans, useLingui } from '@lingui/react/macro';
import { InputGroupField } from '../../../components/SettingsFormField.jsx';

// RANCIMATE: recommended starting point for a heavily insulated Rancilio Silvia
// (see the concept documents 02 and 05). Duty values are percent here.
export const THERMAL_RECOMMENDED = {
  integralBandC: 3.0,
  integralMaxDutyPercent: 12,
  approachBandC: 8.0,
  approachMaxDutyPercent: 35,
  ffScale: 1.0,
  brewHoldoffS: 22,
  brewKpScale: 0.5,
};

function NumberField({ id, label, unit, helpText, step, min, max, formData, onChange, disabled }) {
  return (
    <InputGroupField label={label} htmlFor={id} unit={unit} helpText={helpText} noMargin>
      <input
        id={id}
        name={id}
        type='number'
        step={step}
        min={min}
        max={max}
        className='grow'
        disabled={disabled}
        value={formData[id] ?? ''}
        onChange={onChange(id)}
      />
    </InputGroupField>
  );
}

function WatchdogStatus({ status, threshold }) {
  if (!status) return null;
  const spread = Number(status.lastSpread);
  const hasSpread = Number.isFinite(spread) && spread >= 0;
  const withinBand = hasSpread && spread <= Number(threshold);
  return (
    <div className='bg-base-200 rounded-lg p-3 text-sm' role='status'>
      <div className='grid grid-cols-1 gap-2 sm:grid-cols-3'>
        <div>
          <div className='opacity-70'>
            <Trans>Evaluated shots</Trans>
          </div>
          <div className='font-medium'>{status.shots ?? 0}</div>
        </div>
        <div>
          <div className='opacity-70'>
            <Trans>Last spread</Trans>
          </div>
          <div
            className={`font-medium ${hasSpread ? (withinBand ? 'text-success' : 'text-warning') : ''}`}
          >
            {hasSpread ? `${spread.toFixed(1)} °C` : '–'}
          </div>
        </div>
        <div>
          <div className='opacity-70'>
            <Trans>Stable shots in a row</Trans>
          </div>
          <div className='font-medium'>{status.stableStreak ?? 0}</div>
        </div>
      </div>
      {status.lastAction && (
        <p className='mt-2 font-mono text-xs break-words opacity-80'>{status.lastAction}</p>
      )}
    </div>
  );
}

export function ThermalAdvancedSection({ formData, onChange, setField }) {
  const { t } = useLingui();
  const enabled = !!formData.thermalAdvancedEnabled;
  // The firmware ignores tuning values while the stored watchdog state owns them;
  // keep the fields locked until that state is saved off, so no edit is silently dropped.
  const watchdogOwnsParams = !!formData.thermalWatchdogOwnsParams;
  const watchdogSwitchedOff = watchdogOwnsParams && !formData.thermalWatchdogEnabled;
  const tuningDisabled = !enabled || watchdogOwnsParams;

  const applyRecommended = () => {
    for (const [key, value] of Object.entries(THERMAL_RECOMMENDED)) setField(key, value);
  };

  return (
    <details className='border-base-content/10 border-t pt-4'>
      <summary className='cursor-pointer font-medium'>
        <Trans>PID advanced for isolated boilers</Trans>
      </summary>
      <div className='mt-4 space-y-4'>
        <p className='text-sm opacity-70'>
          <Trans>
            For heavily insulated boilers: limits integral wind-up and heat-up power to prevent
            overshoot, enables the brew feedforward from the heater wattage and holds the integral
            while a shot passes the sensor delay. Turned off, the controller behaves exactly like
            stock GaggiMate.
          </Trans>
        </p>
        <label
          htmlFor='thermalAdvancedEnabled'
          className='label cursor-pointer justify-start gap-3'
        >
          <input
            id='thermalAdvancedEnabled'
            name='thermalAdvancedEnabled'
            type='checkbox'
            className='toggle toggle-primary'
            checked={enabled}
            onChange={onChange('thermalAdvancedEnabled')}
          />
          <span>
            <Trans>Enable advanced control</Trans>
          </span>
        </label>

        <fieldset disabled={!enabled} className='space-y-4'>
          <section className='space-y-3'>
            <h3 className='font-medium'>
              <Trans>Autotune Watchdog</Trans>
            </h3>
            <label
              htmlFor='thermalWatchdogEnabled'
              className='label cursor-pointer justify-start gap-3'
            >
              <input
                id='thermalWatchdogEnabled'
                name='thermalWatchdogEnabled'
                type='checkbox'
                className='toggle toggle-primary'
                checked={!!formData.thermalWatchdogEnabled}
                onChange={onChange('thermalWatchdogEnabled')}
              />
              <span>
                <Trans>Adapt parameters automatically</Trans>
              </span>
            </label>
            <p className='text-sm opacity-70'>
              <Trans>
                Every shot is measured one minute before, during and three minutes after the draw,
                every heat-up is checked for overshoot. While switched on, the firmware adjusts the
                parameters below in small steps after each shot that leaves the band. Switched off,
                the values stay as they are and can be edited; shots are still measured.
              </Trans>
            </p>
            <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
              <NumberField
                id='thermalWatchdogThreshold'
                label={t`Allowed temperature spread`}
                unit='°C'
                step='0.1'
                min='0.3'
                max='10'
                helpText={t`Maximum minus minimum before, during and after a shot.`}
                formData={formData}
                onChange={onChange}
              />
            </div>
            <WatchdogStatus
              status={formData.thermalWatchdogStatus}
              threshold={formData.thermalWatchdogThreshold}
            />
          </section>

          <section className='space-y-3'>
            <h3 className='font-medium'>
              <Trans>Brew feedforward model</Trans>
            </h3>
            <p className='text-sm opacity-70'>
              <Trans>
                The brew temperature always comes from the selected profile. These values only
                describe the machine, so the heater can supply the energy for the incoming cold
                water as soon as water flows, instead of waiting for the sensor, which reports a
                shot about 20 s late.
              </Trans>
            </p>
            <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
              <NumberField
                id='heaterWattage'
                label={t`Heater wattage`}
                unit='W'
                step='10'
                min='300'
                max='3000'
                helpText={t`Rancilio Silvia 230 V: 1100 W. Used for the feedforward when Kff is 0.`}
                formData={formData}
                onChange={onChange}
              />
              <NumberField
                id='incomingWaterTempC'
                label={t`Cold water temperature (tank)`}
                unit='°C'
                step='0.5'
                min='1'
                max='40'
                helpText={t`Temperature of the cold water the pump draws from the tank, not the brew temperature (that comes from the profile). Tells the feedforward how much the incoming water has to be heated.`}
                formData={formData}
                onChange={onChange}
              />
              <NumberField
                id='heatLossWatts'
                label={t`Heat loss during brew`}
                unit='W'
                step='1'
                min='0'
                max='200'
                formData={formData}
                onChange={onChange}
              />
              <NumberField
                id='heaterEfficiency'
                label={t`Heater efficiency`}
                unit='0–1'
                step='0.01'
                min='0.5'
                max='1'
                formData={formData}
                onChange={onChange}
              />
            </div>
          </section>

          <section className='space-y-3'>
            <div className='flex flex-wrap items-center justify-between gap-2'>
              <h3 className='font-medium'>
                <Trans>Control parameters</Trans>
              </h3>
              <button
                type='button'
                className='btn btn-sm btn-outline'
                disabled={tuningDisabled}
                onClick={applyRecommended}
              >
                <Trans>Load recommended values</Trans>
              </button>
            </div>
            {watchdogOwnsParams && !watchdogSwitchedOff && (
              <div className='alert alert-info text-sm' role='note'>
                <Trans>The watchdog manages these values. Switch it off to edit them.</Trans>
              </div>
            )}
            {watchdogSwitchedOff && (
              <div className='alert alert-warning text-sm' role='note'>
                <Trans>Save first: the values become editable once the watchdog is off.</Trans>
              </div>
            )}
            <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
              <NumberField
                id='integralBandC'
                label={t`Integral band`}
                unit='°C'
                step='0.1'
                min='0'
                max='20'
                helpText={t`Integrate only within this distance below setpoint. 0 = off.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='integralMaxDutyPercent'
                label={t`Integral authority`}
                unit='%'
                step='1'
                min='0'
                max='100'
                helpText={t`Cap on the integral share of the heater power. 0 = unlimited.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='approachBandC'
                label={t`Approach band`}
                unit='°C'
                step='0.5'
                min='0'
                max='40'
                helpText={t`Reduce heater power within this distance below setpoint. 0 = off.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='approachMaxDutyPercent'
                label={t`Power at setpoint`}
                unit='%'
                step='1'
                min='5'
                max='100'
                helpText={t`Heater power cap when reaching the setpoint. Never applied during a shot.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='ffScale'
                label={t`Brew feedforward factor`}
                unit='×'
                step='0.01'
                min='0.3'
                max='3'
                helpText={t`Multiplier on the energy the model adds while water flows.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='brewHoldoffS'
                label={t`Integral hold after shot`}
                unit='s'
                step='1'
                min='0'
                max='120'
                helpText={t`Freeze the integral during and this long after a shot (≈ sensor delay). 0 = off.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
              <NumberField
                id='brewKpScale'
                label={t`P factor during shot`}
                unit='×'
                step='0.05'
                min='0.05'
                max='2'
                helpText={t`Softens the reaction to the delayed sensor signal during and after a shot.`}
                formData={formData}
                onChange={onChange}
                disabled={tuningDisabled}
              />
            </div>
          </section>
        </fieldset>
      </div>
    </details>
  );
}
