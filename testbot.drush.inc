<?php

/**
 * @file
 * Provides drush commands.
 *
 * @author Jim Berry ("solotandem", http://drupal.org/user/240748)
 */

/**
 * Implements hook_drush_command().
 */
function testbot_drush_command() {
  $items = array();

  $items['testbot-drupal-patch'] = array(
    'description' => dt('Submit a drupal core patch for testing.'),
    'options' => array(
      'branch' => dt('Core branch to test with patch.'),
      'issue' => dt('Node ID of core issue that patch relates to.'),
      'php-version' => dt('PHP version with which to run tests.'),
      'database' => dt('Database (name and version) with which to run tests.'),
      'patch' => dt('URL of patch file.'),
      'properties' => dt('Valid json string with properties array. If omitted, properties will be read from any ".testbot" file in the repository root. May include any of the other parameters, but, if present, the individual parameters will override.'),
    ),
    'examples' => array(
      'drush testbot --branch="7.x" --php-version="5.4" --database="mariadb-5.5"' => 'Submit the "working directory" patch (based on git diff in current working directory) against drupal 7.x branch; test with php 5.4 / mariadb 5.5 environment.',
      'drush testbot --properties="{"branch":"7.x","php-version":"5.4","database":"mariadb-5.5"}"' => 'Same command using --properties parameter.',
      'drush testbot --properties="{"php-version":"5.6","database":"mongodb-2.6"}"' => 'Submit the "working directory" patch against drupal 8.0.x branch; test with php 5.6 / mongodb 2.6 environment.',
      'drush testbot --patch="<URL of patch file>"' => 'Submit the patch at "<URL of patch file>" against drupal 8.0.x branch; test with default environment.',
    ),
    'aliases' => array('testbot'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUSH,
  );

  return $items;
}

/**
 * Submits a drupal core patch for testing.
 */
function drush_testbot_drupal_patch() {
  $properties = ($value = drush_get_option('properties')) ? $value : '';
  if (empty($properties)) {
    // Read parameters from .testbot file in repository root.
    $repository_root = shell_exec('git rev-parse --show-toplevel');
    $repository_root = trim($repository_root);
    if (empty($repository_root)) {
      return drush_set_error('git', dt('Repository root not found. Is this a git repository?'));
    }
    $filename = $repository_root . '/.testbot';
    $properties = file_exists($filename) ? file_get_contents($filename) : '[]';
  }

  $decode = json_decode($properties, TRUE);
  if (!is_array($decode) || is_null($decode)) {
    return drush_set_error('properties', dt('Invalid properties "@properties": must be in JSON format.', array('@properties' => $properties)));
  }
  $properties = $decode;

  // Override properties with individual parameters.
  foreach (array('branch', 'issue', 'php-version', 'database', 'patch') as $name) {
    drush_testbot_parameter_merge($name, $properties);
  }

  if (empty($properties['issue'])) {
    // Extract issue number from repository branch name.
    // Assume branch name follows pattern of issue-description.
    // See https://www.drupal.org/node/1054616.
    $repository_branch = shell_exec('git rev-parse --abbrev-ref HEAD');
    $repository_branch = trim($repository_branch);
    if (empty($repository_branch)) {
      return drush_set_error('git', dt('Repository name not found. Is this a git repository?'));
    }
    list($properties['issue'], ) = explode('-', $repository_branch, 2);
  }

  if (empty($properties['patch'])) {
    // Create patch file.
    $patch_url = drush_testbot_patch_create();
    if (empty($patch_url)) {
      return drush_set_error('diff', dt('Patch file not created. Is this a git repository?'));
    }
    $properties['patch'] = $patch_url;
    echo "Patch file: $patch_url\n";
  }

  // Gather data for transfer.
  $url = 'https://reviewdriven.com/api/1.0/patch/submit';
  $headers = array('Content-Type: application/json', 'Accept: application/json');
  $data = array(
    'properties' => $properties,
  );

  $options = array(
    'url' => $url,
    'headers' => $headers,
    'data' => json_encode($data),
  );

  // Submit HTTP request.
  $response = testbot_http_request($options, 'Submit to reviewdriven');
  if ($response === FALSE || empty($response)) {
    return;
  }

  $data = json_decode($response, TRUE);
  if (is_null($data)) {
    return;
  }

  $data += array(
    'errors' => array(),
    'messages' => array(),
  );
  foreach ($data as $type => $messages) {
    foreach ($messages as $message) {
      echo $message . "\n";
    }
  }
}

/**
 * Merges properties array with individual command line parameters.
 *
 * @param string $name
 *   Property name.
 * @param array $properties
 *   Associative array of properties.
 */
function drush_testbot_parameter_merge($name, &$properties) {
  if ($value = drush_get_option($name)) {
    $value = trim(trim($value, "'"));
    $properties[$name] = $value;
  }
}

/**
 * Submits a patch to pastebin.
 *
 * @return string
 *   Patch URL.
 */
function drush_testbot_patch_create() {
  $patch_url = '';

  // Create patch file.
  if (!($diff = shell_exec('git diff'))) {
    drush_set_error('diff', dt('No valid result from diff command. Is this a git repository?'));
    return $patch_url;
  }

  // Gather data for transfer.
  $pastebin_url = 'https://paste.kde.org';
  $path = 'api/json/create';
  $url = "$pastebin_url/$path";
  $headers = array('Content-Type: application/json', 'Accept: application/json');
  $data = array(
    'data' => $diff,
    'language' => 'diff',
    'private' => 'false',
    'expire' => '86400', // 1 day
//     'expire' => '1800', // 30 mins
  );
  $options = array(
    'url' => $url,
    'headers' => $headers,
    'data' => json_encode($data),
  );

  // Submit HTTP request.
  $response = testbot_http_request($options, 'Upload to pastebin');
  if ($response === FALSE) {
    return $patch_url;
  }

  $response = json_decode($response);
  if (empty($response->result->id)) {
    return $patch_url;
  }

  $patch_url = "$pastebin_url/{$response->result->id}/{$response->result->hash}/raw";
  return $patch_url;
}

/**
 * Processes an HTTP request.
 *
 * @param array $options
 *   Associative array with keys of: url, headers, data.
 * @param string $task
 *   Task string to use in error messages.
 *
 * @return string|boolean
 *   If successful, the response in JSON format; FALSE otherwise.
 */
function testbot_http_request($options, $task) {
  // Initialize the session.
  $ch = curl_init();

  // Set the transfer parameters.
  curl_setopt($ch, CURLOPT_URL, $options['url']);
  curl_setopt($ch, CURLOPT_HTTPHEADER, $options['headers']);
  curl_setopt($ch, CURLOPT_POST, TRUE);
  curl_setopt($ch, CURLOPT_POSTFIELDS, $options['data']);
  curl_setopt($ch, CURLOPT_RETURNTRANSFER, TRUE);

  // Execute the post request.
  $response = curl_exec($ch);
  $info = curl_getinfo($ch);

  // Close the session.
  curl_close($ch);

  // Process the response.
  if ($info['http_code'] != 200) {
    $message = dt('!task failed with code !code', array('!task' => $task, '!code' => $info['http_code']));
    echo $message . "\n";
  }
  elseif ($response === FALSE) {
    $message = dt('!task failed.', array('!task' => $task));
    echo $message . "\n";
  }

  return $response;
}
